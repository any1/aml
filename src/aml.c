/*
 * Copyright (c) 2020 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <poll.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>

#include "aml.h"
#include "backend.h"
#include "sys/queue.h"
#include "thread-pool.h"

#define EXPORT __attribute__((visibility("default")))

#define EVENT_MASK_DEFAULT AML_EVENT_READ

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

enum aml_obj_type {
	AML_OBJ_UNSPEC = 0,
	AML_OBJ_AML,
	AML_OBJ_HANDLER,
	AML_OBJ_TIMER,
	AML_OBJ_TICKER,
	AML_OBJ_SIGNAL,
	AML_OBJ_WORK,
	AML_OBJ_IDLE,
};

struct aml_obj {
	enum aml_obj_type type;
	int ref;
	void* userdata;
	aml_free_fn free_fn;
	aml_callback_fn cb;
	unsigned long long id;

	void* backend_data;

	LIST_ENTRY(aml_obj) link;
	LIST_ENTRY(aml_obj) global_link;
	TAILQ_ENTRY(aml_obj) event_link;
};

LIST_HEAD(aml_obj_list, aml_obj);
TAILQ_HEAD(aml_obj_queue, aml_obj);

struct aml_handler {
	struct aml_obj obj;

	int fd;
	enum aml_event event_mask;
	atomic_uint revents;

	struct aml* parent;
};

struct aml_timer {
	struct aml_obj obj;

	uint32_t timeout;
	uint64_t deadline;

	LIST_ENTRY(aml_timer) link;
};

LIST_HEAD(aml_timer_list, aml_timer);

struct aml_signal {
	struct aml_obj obj;

	int signo;
};

struct aml_work {
	struct aml_obj obj;

	aml_callback_fn work_fn;
};

struct aml_idle {
	struct aml_obj obj;

	LIST_ENTRY(aml_idle) link;
};

LIST_HEAD(aml_idle_list, aml_idle);

struct aml {
	struct aml_obj obj;

	void* state;
	struct aml_backend backend;

	int self_pipe_rfd, self_pipe_wfd;

	bool do_exit;

	struct aml_obj_list obj_list;
	pthread_mutex_t obj_list_mutex;

	struct aml_timer_list timer_list;
	pthread_mutex_t timer_list_mutex;

	struct aml_idle_list idle_list;

	struct aml_obj_queue event_queue;
	pthread_mutex_t event_queue_mutex;

	bool have_thread_pool;
};

static struct aml* aml__default = NULL;

static unsigned long long aml__obj_id = 0;
static struct aml_obj_list aml__obj_list = LIST_HEAD_INITIALIZER(aml__obj_list);

// TODO: Properly initialise this?
static pthread_mutex_t aml__ref_mutex;

extern struct aml_backend implementation;

static struct aml_timer* aml__get_timer_with_earliest_deadline(struct aml* self);

#if defined(GIT_VERSION)
EXPORT const char aml_version[] = GIT_VERSION;
#elif defined(PROJECT_VERSION)
EXPORT const char aml_version[] = PROJECT_VERSION;
#else
EXPORT const char aml_version[] = "UNKNOWN";
#endif

EXPORT
void aml_set_default(struct aml* aml)
{
	aml__default = aml;
}

EXPORT
struct aml* aml_get_default(void)
{
	return aml__default;
}

static int aml__poll(struct aml* self, int timeout)
{
	return self->backend.poll(self->state, timeout);
}

static int aml__add_fd(struct aml* self, struct aml_handler* handler)
{
	return self->backend.add_fd(self->state, handler);
}

static int aml__del_fd(struct aml* self, struct aml_handler* handler)
{
	return self->backend.del_fd(self->state, handler);
}

static int aml__mod_fd(struct aml* self, struct aml_handler* handler)
{
	if (!self->backend.mod_fd) {
		aml__del_fd(self, handler);
		return aml__add_fd(self, handler);
	}

	return self->backend.mod_fd(self->state, handler);
}

static int aml__set_deadline(struct aml* self, uint64_t deadline)
{
	return self->backend.set_deadline(self->state, deadline);
}

static void aml__post_dispatch(struct aml* self)
{
	if (self->backend.post_dispatch)
		self->backend.post_dispatch(self->state);
}

static void aml__dont_block(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static uint64_t aml__gettime_ms(struct aml* self)
{
	struct timespec ts = { 0 };
	clock_gettime(self->backend.clock, &ts);
	return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void aml__ref_lock(void)
{
	pthread_mutex_lock(&aml__ref_mutex);
}

static void aml__ref_unlock(void)
{
	pthread_mutex_unlock(&aml__ref_mutex);
}

static void aml__obj_global_ref(struct aml_obj* obj)
{
	aml__ref_lock();
	obj->id = aml__obj_id++;
	LIST_INSERT_HEAD(&aml__obj_list, obj, global_link);
	aml__ref_unlock();
}

static void on_self_pipe_read(void* obj) {
	struct aml* self = aml_get_userdata(obj);
	assert(self);
	assert(self->self_pipe_rfd == aml_get_fd(obj));

	char dummy[256];
	while (read(self->self_pipe_rfd, dummy, sizeof(dummy)) > 0);
}

static void aml__destroy_self_pipe(void* userdata)
{
	struct aml* self = userdata;

	close(self->self_pipe_rfd);
	close(self->self_pipe_wfd);
}

static int aml__init_self_pipe(struct aml* self)
{
	if (self->backend.interrupt)
		return 0;

	int fds[2];
	if (pipe(fds) < 0)
		return -1;

	aml__dont_block(fds[0]);
	aml__dont_block(fds[1]);

	self->self_pipe_rfd = fds[0];
	self->self_pipe_wfd = fds[1];

	struct aml_handler* handler =
		aml_handler_new(self->self_pipe_rfd, on_self_pipe_read, self,
		                aml__destroy_self_pipe);
	if (!handler)
		goto failure;

	aml_start(self, handler);
	aml_unref(handler);

	return 0;

failure:
	close(fds[1]);
	close(fds[0]);
	return -1;
}

EXPORT
void aml_interrupt(struct aml* self)
{
	if (self->backend.interrupt) {
		self->backend.interrupt(self->state);
		return;
	}

	char one = 1;
	write(self->self_pipe_wfd, &one, sizeof(one));
}

EXPORT
struct aml* aml_new(void)
{
	struct aml* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_AML;
	self->obj.ref = 1;

	LIST_INIT(&self->obj_list);
	LIST_INIT(&self->timer_list);
	LIST_INIT(&self->idle_list);
	TAILQ_INIT(&self->event_queue);

	pthread_mutex_init(&self->event_queue_mutex, NULL);
	pthread_mutex_init(&self->obj_list_mutex, NULL);
	pthread_mutex_init(&self->timer_list_mutex, NULL);

	memcpy(&self->backend, &implementation, sizeof(self->backend));

	if (!self->backend.thread_pool_acquire)
		self->backend.thread_pool_acquire = thread_pool_acquire_default;
	if (!self->backend.thread_pool_release)
		self->backend.thread_pool_release = thread_pool_release_default;
	if (!self->backend.thread_pool_enqueue)
		self->backend.thread_pool_enqueue = thread_pool_enqueue_default;

	self->state = self->backend.new_state(self);
	if (!self->state)
		goto failure;

	if (aml__init_self_pipe(self) < 0)
		goto pipe_failure;

	aml__obj_global_ref(&self->obj);

	return self;

pipe_failure:
	self->backend.del_state(self->state);
failure:
	free(self);
	return NULL;
}

static int get_n_processors(void)
{
#ifdef _SC_NPROCESSORS_ONLN
	return sysconf(_SC_NPROCESSORS_ONLN);
#else
	return 4; /* Guess */
#endif
}

EXPORT
int aml_require_workers(struct aml* self, int n)
{
	if (n < 0)
		n = get_n_processors();

	if (self->backend.thread_pool_acquire(self, n) < 0)
		return -1;

	self->have_thread_pool = true;
	return 0;
}

EXPORT
struct aml_handler* aml_handler_new(int fd, aml_callback_fn callback,
                                    void* userdata, aml_free_fn free_fn)
{
	struct aml_handler* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_HANDLER;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;
	self->obj.cb = callback;

	self->fd = fd;
	self->event_mask = EVENT_MASK_DEFAULT;

	aml__obj_global_ref(&self->obj);

	return self;
}

EXPORT
struct aml_timer* aml_timer_new(uint32_t timeout, aml_callback_fn callback,
                                void* userdata, aml_free_fn free_fn)
{
	struct aml_timer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_TIMER;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;
	self->obj.cb = callback;

	self->timeout = timeout;

	aml__obj_global_ref(&self->obj);

	return self;
}

EXPORT
struct aml_ticker* aml_ticker_new(uint32_t period, aml_callback_fn callback,
                                  void* userdata, aml_free_fn free_fn)
{
	struct aml_timer* timer =
		aml_timer_new(period, callback, userdata, free_fn);
	timer->obj.type = AML_OBJ_TICKER;
	return (struct aml_ticker*)timer;
}

EXPORT
struct aml_signal* aml_signal_new(int signo, aml_callback_fn callback,
                                  void* userdata, aml_free_fn free_fn)
{
	struct aml_signal* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_SIGNAL;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;
	self->obj.cb = callback;

	self->signo = signo;

	aml__obj_global_ref(&self->obj);

	return self;
}

EXPORT
struct aml_work* aml_work_new(aml_callback_fn work_fn, aml_callback_fn callback,
                              void* userdata, aml_free_fn free_fn)
{
	struct aml_work* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_WORK;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;
	self->obj.cb = callback;

	self->work_fn = work_fn;

	aml__obj_global_ref(&self->obj);

	return self;
}

EXPORT
struct aml_idle* aml_idle_new(aml_callback_fn callback, void* userdata,
                              aml_free_fn free_fn)
{
	struct aml_idle* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_IDLE;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;
	self->obj.cb = callback;

	aml__obj_global_ref(&self->obj);

	return self;
}

static bool aml__obj_is_started_unlocked(struct aml* self, void* obj)
{
	struct aml_obj* elem;
	LIST_FOREACH(elem, &self->obj_list, link)
		if (elem == obj)
			return true;

	return false;
}

EXPORT
bool aml_is_started(struct aml* self, void* obj)
{
	pthread_mutex_lock(&self->obj_list_mutex);
	bool result = aml__obj_is_started_unlocked(self, obj);
	pthread_mutex_unlock(&self->obj_list_mutex);
	return result;
}

static int aml__obj_try_add(struct aml* self, void* obj)
{
	int rc = -1;

	pthread_mutex_lock(&self->obj_list_mutex);

	if (!aml__obj_is_started_unlocked(self, obj)) {
		aml_ref(obj);
		LIST_INSERT_HEAD(&self->obj_list, (struct aml_obj*)obj, link);
		rc = 0;
	}

	pthread_mutex_unlock(&self->obj_list_mutex);

	return rc;
}

static void aml__obj_remove_unlocked(struct aml* self, void* obj)
{
	LIST_REMOVE((struct aml_obj*)obj, link);
	aml_unref(obj);
}

static void aml__obj_remove(struct aml* self, void* obj)
{
	pthread_mutex_lock(&self->obj_list_mutex);
	aml__obj_remove_unlocked(self, obj);
	pthread_mutex_unlock(&self->obj_list_mutex);
}

static int aml__obj_try_remove(struct aml* self, void* obj)
{
	int rc = -1;

	pthread_mutex_lock(&self->obj_list_mutex);

	if (aml__obj_is_started_unlocked(self, obj)) {
		aml__obj_remove_unlocked(self, obj);
		rc = 0;
	}

	pthread_mutex_unlock(&self->obj_list_mutex);

	return rc;
}

static int aml__start_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__add_fd(self, handler) < 0)
		return -1;

	handler->parent = self;

	return 0;
}

static int aml__start_timer(struct aml* self, struct aml_timer* timer)
{
	timer->deadline = aml__gettime_ms(self) + timer->timeout;

	pthread_mutex_lock(&self->timer_list_mutex);
	LIST_INSERT_HEAD(&self->timer_list, timer, link);
	pthread_mutex_unlock(&self->timer_list_mutex);

	if (timer->timeout == 0) {
		assert(timer->obj.type != AML_OBJ_TICKER);
		aml_stop(self, timer);
		aml_emit(self, timer, 0);
		aml_interrupt(self);
		return 0;
	}

	struct aml_timer* earliest = aml__get_timer_with_earliest_deadline(self);
	if (earliest == timer)
		aml__set_deadline(self, timer->deadline);

	return 0;
}

static int aml__start_signal(struct aml* self, struct aml_signal* sig)
{
	return self->backend.add_signal(self->state, sig);
}

static int aml__start_work(struct aml* self, struct aml_work* work)
{
	return self->backend.thread_pool_enqueue(self, work);
}

static int aml__start_idle(struct aml* self, struct aml_idle* idle)
{
	LIST_INSERT_HEAD(&self->idle_list, idle, link);
	return 0;
}

static int aml__start_unchecked(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__start_handler(self, obj);
	case AML_OBJ_TIMER: /* fallthrough */
	case AML_OBJ_TICKER: return aml__start_timer(self, obj);
	case AML_OBJ_SIGNAL: return aml__start_signal(self, obj);
	case AML_OBJ_WORK: return aml__start_work(self, obj);
	case AML_OBJ_IDLE: return aml__start_idle(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

EXPORT
int aml_start(struct aml* self, void* obj)
{
	if (aml__obj_try_add(self, obj) < 0)
		return -1;

	if (aml__start_unchecked(self, obj) == 0)
		return 0;

	aml__obj_remove(self, obj);
	return -1;
}

static int aml__stop_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__del_fd(self, handler) < 0)
		return -1;

	handler->parent = NULL;

	return 0;
}

static int aml__stop_timer(struct aml* self, struct aml_timer* timer)
{
	pthread_mutex_lock(&self->timer_list_mutex);
	LIST_REMOVE(timer, link);
	pthread_mutex_unlock(&self->timer_list_mutex);
	return 0;
}

static int aml__stop_signal(struct aml* self, struct aml_signal* sig)
{
	return self->backend.del_signal(self->state, sig);
}

static int aml__stop_work(struct aml* self, struct aml_work* work)
{
	/* Note: The cb may be executed anyhow */
	return 0;
}

static int aml__stop_idle(struct aml* self, struct aml_idle* idle)
{
	LIST_REMOVE(idle, link);
	return 0;
}

static int aml__stop_unchecked(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__stop_handler(self, obj);
	case AML_OBJ_TIMER: /* fallthrough */
	case AML_OBJ_TICKER: return aml__stop_timer(self, obj);
	case AML_OBJ_SIGNAL: return aml__stop_signal(self, obj);
	case AML_OBJ_WORK: return aml__stop_work(self, obj);
	case AML_OBJ_IDLE: return aml__stop_idle(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

EXPORT
int aml_stop(struct aml* self, void* obj)
{
	aml_ref(obj);

	if (aml__obj_try_remove(self, obj) >= 0)
		aml__stop_unchecked(self, obj);

	aml_unref(obj);

	return 0;
}

static struct aml_timer* aml__get_timer_with_earliest_deadline(struct aml* self)
{
	uint64_t deadline = UINT64_MAX;
	struct aml_timer* result = NULL;

	struct aml_timer* timer;

	pthread_mutex_lock(&self->timer_list_mutex);
	LIST_FOREACH(timer, &self->timer_list, link)
		if (timer->deadline < deadline) {
			deadline = timer->deadline;
			result = timer;
		}
	pthread_mutex_unlock(&self->timer_list_mutex);

	return result;
}

static bool aml__handle_timeout(struct aml* self, uint64_t now)
{
	struct aml_timer* timer = aml__get_timer_with_earliest_deadline(self);
	if (!timer || timer->deadline > now)
		return false;

	aml_emit(self, timer, 0);

	switch (timer->obj.type) {
	case AML_OBJ_TIMER:
		aml_stop(self, timer);
		break;
	case AML_OBJ_TICKER:
		timer->deadline += timer->timeout;
		break;
	default:
		abort();
		break;
	}

	return true;
}

static void aml__handle_idle(struct aml* self)
{
	struct aml_idle* idle;

	LIST_FOREACH(idle, &self->idle_list, link)
		if (idle->obj.cb)
			idle->obj.cb(idle);
}

static void aml__handle_event(struct aml* self, struct aml_obj* obj)
{
	/* A reference is kept here in case an object is stopped inside the
	 * callback. We want the object to live until we're done with it.
	 */
	aml_ref(obj);

	if (obj->cb)
		obj->cb(obj);

	if (obj->type == AML_OBJ_HANDLER) {
		struct aml_handler* handler = (struct aml_handler*)obj;
		handler->revents = 0;

		if (self->backend.flags & AML_BACKEND_EDGE_TRIGGERED)
			aml__mod_fd(self, handler);
	}

	aml_unref(obj);
}

/* Might exit earlier than timeout. It's up to the user to check */
EXPORT
int aml_poll(struct aml* self, int timeout)
{
	return aml__poll(self, timeout);
}

static struct aml_obj* aml__event_dequeue(struct aml* self)
{
	pthread_mutex_lock(&self->event_queue_mutex);
	struct aml_obj* obj = TAILQ_FIRST(&self->event_queue);
	if (obj)
		TAILQ_REMOVE(&self->event_queue, obj, event_link);
	pthread_mutex_unlock(&self->event_queue_mutex);
	return obj;
}

EXPORT
void aml_dispatch(struct aml* self)
{
	uint64_t now = aml__gettime_ms(self);
	while (aml__handle_timeout(self, now));

	struct aml_timer* earliest = aml__get_timer_with_earliest_deadline(self);
	if (earliest) {
		assert(earliest->deadline > now);
		aml__set_deadline(self, earliest->deadline);
	}

	sigset_t sig_old, sig_new;
	sigfillset(&sig_new);

	pthread_sigmask(SIG_BLOCK, &sig_new, &sig_old);

	struct aml_obj* obj;
	while ((obj = aml__event_dequeue(self)) != NULL) {
		aml__handle_event(self, obj);
		aml_unref(obj);
	}

	pthread_sigmask(SIG_SETMASK, &sig_old, NULL);

	aml__handle_idle(self);
	aml__post_dispatch(self);
}

EXPORT
int aml_run(struct aml* self)
{
	self->do_exit = false;

	do {
		aml_poll(self, -1);
		aml_dispatch(self);
	} while (!self->do_exit);

	return 0;
}

EXPORT
void aml_exit(struct aml* self)
{
	self->do_exit = true;

	if (self->backend.exit)
		self->backend.exit(self->state);
}

EXPORT
int aml_ref(void* obj)
{
	struct aml_obj* self = obj;

	aml__ref_lock();
	int ref = self->ref++;
	aml__ref_unlock();

	return ref;
}

static void aml__free(struct aml* self)
{
	while (!LIST_EMPTY(&self->obj_list)) {
		struct aml_obj* obj = LIST_FIRST(&self->obj_list);

		aml__stop_unchecked(self, obj);
		aml__obj_remove_unlocked(self, obj);
	}

	if (self->have_thread_pool)
		self->backend.thread_pool_release(self);

	self->backend.del_state(self->state);

	while (!TAILQ_EMPTY(&self->event_queue)) {
		struct aml_obj* obj = TAILQ_FIRST(&self->event_queue);
		TAILQ_REMOVE(&self->event_queue, obj, event_link);
		aml_unref(obj);
	}

	pthread_mutex_destroy(&self->timer_list_mutex);
	pthread_mutex_destroy(&self->obj_list_mutex);
	pthread_mutex_destroy(&self->event_queue_mutex);

	free(self);
}

static void aml__free_handler(struct aml_handler* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

static void aml__free_timer(struct aml_timer* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

static void aml__free_signal(struct aml_signal* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

static void aml__free_work(struct aml_work* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

static void aml__free_idle(struct aml_idle* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

EXPORT
int aml_unref(void* obj)
{
	struct aml_obj* self = obj;

	aml__ref_lock();
	int ref = --self->ref;
	if (ref == 0)
		LIST_REMOVE(self, global_link);
	aml__ref_unlock();
	assert(ref >= 0);
	if (ref > 0)
		goto done;

	switch (self->type) {
	case AML_OBJ_AML:
		aml__free(obj);
		break;
	case AML_OBJ_HANDLER:
		aml__free_handler(obj);
		break;
	case AML_OBJ_TIMER:
		/* fallthrough */
	case AML_OBJ_TICKER:
		aml__free_timer(obj);
		break;
	case AML_OBJ_SIGNAL:
		aml__free_signal(obj);
		break;
	case AML_OBJ_WORK:
		aml__free_work(obj);
		break;
	case AML_OBJ_IDLE:
		aml__free_idle(obj);
		break;
	default:
		abort();
		break;
	}

done:
	return ref;
}

EXPORT
unsigned long long aml_get_id(const void* obj)
{
	const struct aml_obj* aml_obj = obj;
	return aml_obj->id;
}

EXPORT
void* aml_try_ref(unsigned long long id)
{
	struct aml_obj* obj = NULL;

	aml__ref_lock();
	LIST_FOREACH(obj, &aml__obj_list, global_link)
		if (obj->id == id)
			break;

	if (obj && obj->id == id)
		obj->ref++;
	else
		obj = NULL;

	aml__ref_unlock();
	return obj;
}

EXPORT
void* aml_get_userdata(const void* obj)
{
	const struct aml_obj* aml_obj = obj;
	return aml_obj->userdata;
}

EXPORT
void aml_set_userdata(void* obj, void* userdata, aml_free_fn free_fn)
{
	struct aml_obj* aml_obj = obj;
	aml_obj->userdata = userdata;
	aml_obj->free_fn = free_fn;
}

void aml_emit(struct aml* self, void* ptr, uint32_t revents)
{
	struct aml_obj* obj = ptr;

	if (obj->type == AML_OBJ_HANDLER) {
		struct aml_handler* handler = ptr;
		uint32_t old = atomic_fetch_or(&handler->revents, revents);
		if (old != 0)
			return;
	}

	sigset_t sig_old, sig_new;
	sigfillset(&sig_new);

	pthread_sigmask(SIG_BLOCK, &sig_new, &sig_old);
	pthread_mutex_lock(&self->event_queue_mutex);
	TAILQ_INSERT_TAIL(&self->event_queue, obj, event_link);
	aml_ref(obj);
	pthread_mutex_unlock(&self->event_queue_mutex);
	pthread_sigmask(SIG_SETMASK, &sig_old, NULL);
}

EXPORT
enum aml_event aml_get_event_mask(const struct aml_handler* handler)
{
	return handler->event_mask;
}

EXPORT
void aml_set_event_mask(struct aml_handler* handler, enum aml_event mask)
{
	handler->event_mask = mask;

	if (handler->parent && aml_is_started(handler->parent, handler))
		aml__mod_fd(handler->parent, handler);
}

EXPORT
enum aml_event aml_get_revents(const struct aml_handler* handler)
{
	return handler->revents;
}

EXPORT
int aml_get_fd(const void* ptr)
{
	const struct aml_obj* obj = ptr;

	switch (obj->type) {
	case AML_OBJ_AML:;
		const struct aml* aml = ptr;
		return aml->backend.get_fd ?
			aml->backend.get_fd(aml->state) : -1;
	case AML_OBJ_HANDLER:
		return ((struct aml_handler*)ptr)->fd;
	default:
		break;
	}

	return -1;
}

EXPORT
int aml_get_signo(const struct aml_signal* sig)
{
	return sig->signo;
}

aml_callback_fn aml_get_work_fn(const struct aml_work* work)
{
	return work->work_fn;
}

void* aml_get_backend_data(const void* ptr)
{
	const struct aml_obj* obj = ptr;
	return obj->backend_data;
}

void aml_set_backend_data(void* ptr, void* data)
{
	struct aml_obj* obj = ptr;
	obj->backend_data = data;
}

void* aml_get_backend_state(const struct aml* self)
{
	return self->state;
}

EXPORT
void aml_set_duration(void* ptr, uint32_t duration)
{
	struct aml_obj* obj = ptr;

	switch (obj->type) {
	case AML_OBJ_TIMER: /* fallthrough */
	case AML_OBJ_TICKER:
		((struct aml_timer*)ptr)->timeout = duration;
		return;
	default:
		break;
	}

	abort();
}
