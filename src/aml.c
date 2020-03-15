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
#include "sys/queue.h"

#define EXPORT __attribute__((visibility("default")))

#define EVENT_MASK_DEFAULT (POLLIN | POLLPRI)

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
};

struct aml_obj {
	enum aml_obj_type type;
	atomic_int ref;
	void* userdata;
	aml_free_fn free_fn;
	aml_callback_fn cb;

	int pending;

	LIST_ENTRY(aml_obj) link;
	TAILQ_ENTRY(aml_obj) event_link;
};

LIST_HEAD(aml_obj_list, aml_obj);
TAILQ_HEAD(aml_obj_queue, aml_obj);

struct aml_handler {
	struct aml_obj obj;

	int fd;
	uint32_t event_mask;
	uint32_t revents;

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

struct aml {
	struct aml_obj obj;

	void* state;
	struct aml_backend backend;

	bool do_exit;

	struct aml_obj_list obj_list;
	pthread_mutex_t obj_list_mutex;

	struct aml_timer_list timer_list;

	struct aml_obj_queue event_queue;
	pthread_mutex_t event_queue_mutex;
};

static struct aml* aml__default = NULL;

extern struct aml_backend posix_backend;

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
	return self->backend.mod_fd(self->state, handler);
}

void aml__dont_block(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static uint64_t gettime_ms(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

EXPORT
struct aml* aml_new(const struct aml_backend* backend, size_t backend_size)
{
	struct aml* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_AML;
	self->obj.ref = 1;

	LIST_INIT(&self->obj_list);
	LIST_INIT(&self->timer_list);
	TAILQ_INIT(&self->event_queue);

	pthread_mutex_init(&self->event_queue_mutex, NULL);
	pthread_mutex_init(&self->obj_list_mutex, NULL);

	if (backend_size > sizeof(self->backend))
		return NULL;

	if (backend)
		memcpy(&self->backend, backend, backend_size);
	else
		memcpy(&self->backend, &posix_backend, sizeof(self->backend));

	self->state = self->backend.new_state(self);
	if (!self->state)
		goto failure;

	return self;

failure:
	free(self);
	return NULL;
}

EXPORT
int aml_require_workers(struct aml* self, int n)
{
	return self->backend.init_thread_pool(self->state, n);
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

	return self;
}

void aml__obj_ref(struct aml* self, void* obj)
{
	pthread_mutex_lock(&self->obj_list_mutex);
	aml_ref(obj);
	LIST_INSERT_HEAD(&self->obj_list, (struct aml_obj*)obj, link);
	pthread_mutex_unlock(&self->obj_list_mutex);
}

void aml__obj_unref(struct aml* self, void* obj)
{
	pthread_mutex_lock(&self->obj_list_mutex);
	LIST_REMOVE((struct aml_obj*)obj, link);
	aml_unref(obj);
	pthread_mutex_unlock(&self->obj_list_mutex);
}

int aml__start_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__add_fd(self, handler) < 0)
		return -1;

	handler->parent = self;
	aml__obj_ref(self, handler);

	return 0;
}

static bool aml__is_timer_started(struct aml* self, struct aml_timer* timer)
{
	struct aml_timer* node;
	LIST_FOREACH(node, &self->timer_list, link)
		if (node == timer)
			return true;

	return false;
}

int aml__start_timer(struct aml* self, struct aml_timer* timer)
{
	if (aml__is_timer_started(self, timer))
		return -1;

	aml__obj_ref(self, timer);

	timer->deadline = gettime_ms() + timer->timeout;
	LIST_INSERT_HEAD(&self->timer_list, timer, link);

	return 0;
}

int aml__start_signal(struct aml* self, struct aml_signal* sig)
{
	if (self->backend.add_signal(self->state, sig) < 0)
		return -1;

	aml__obj_ref(self, sig);

	return 0;
}

int aml__start_work(struct aml* self, struct aml_work* work)
{
	aml__obj_ref(self, work);

	if (self->backend.enqueue_work(self->state, work) == 0)
		return 0;

	aml__obj_unref(self, work);
	return -1;
}

EXPORT
int aml_start(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__start_handler(self, obj);
	case AML_OBJ_TIMER: /* fallthrough */
	case AML_OBJ_TICKER: return aml__start_timer(self, obj);
	case AML_OBJ_SIGNAL: return aml__start_signal(self, obj);
	case AML_OBJ_WORK: return aml__start_work(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

int aml__stop_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__del_fd(self, handler) < 0)
		return -1;

	handler->parent = NULL;
	aml__obj_unref(self, handler);

	return 0;
}

int aml__stop_timer(struct aml* self, struct aml_timer* timer)
{
	if (!aml__is_timer_started(self, timer))
		return -1;

	LIST_REMOVE(timer, link);
	aml__obj_unref(self, timer);

	return 0;
}

int aml__stop_signal(struct aml* self, struct aml_signal* sig)
{
	if (self->backend.del_signal(self->state, sig) < 0)
		return -1;

	aml__obj_unref(self, sig);

	return 0;
}

int aml__stop_work(struct aml* self, struct aml_work* work)
{
	/* Note: The cb may be executed anyhow */
	aml__obj_unref(self, work);
	return 0;
}

EXPORT
int aml_stop(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__stop_handler(self, obj);
	case AML_OBJ_TIMER: /* fallthrough */
	case AML_OBJ_TICKER: return aml__stop_timer(self, obj);
	case AML_OBJ_SIGNAL: return aml__stop_signal(self, obj);
	case AML_OBJ_WORK: return aml__stop_work(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

struct aml_timer* aml__get_timer_with_earliest_deadline(struct aml* self)
{
	uint64_t deadline = UINT64_MAX;
	struct aml_timer* result = NULL;

	struct aml_timer* timer;
	LIST_FOREACH(timer, &self->timer_list, link)
		if (timer->deadline < deadline) {
			deadline = timer->deadline;
			result = timer;
		}

	return result;
}

int aml__get_next_timeout(struct aml* self, int timeout)
{
	struct aml_timer* timer = aml__get_timer_with_earliest_deadline(self);
	if (!timer)
		return timeout;

	uint64_t now = gettime_ms();
	if (timer->deadline <= now)
		return 0;

	int timer_timeout = timer->deadline - now;

	return timeout < 0 ? timer_timeout : MIN(timeout, timer_timeout);
}

void aml__handle_timeout(struct aml* self)
{
	struct aml_timer* timer = aml__get_timer_with_earliest_deadline(self);

	uint64_t now = gettime_ms();

	if (!timer || timer->deadline > now)
		return;

	aml_emit(self, timer, 0);

	switch (timer->obj.type) {
	case AML_OBJ_TIMER:
		aml__stop_timer(self, timer);
		break;
	case AML_OBJ_TICKER:
		timer->deadline += timer->timeout;
		break;
	default:
		abort();
		break;
	}
}


void aml__handle_event(struct aml* self, struct aml_obj* obj)
{
	/* A reference is kept here in case an object is stopped inside the
	 * callback. We want the object to live until we're done with it.
	 */
	aml_ref(obj);

	if (obj->cb)
		obj->cb(obj);

	if (obj->type == AML_OBJ_HANDLER)
		((struct aml_handler*)obj)->revents = 0;

	obj->pending = 0;

	aml_unref(obj);
}

/* Might exit earlier than timeout. It's up to the user to check */
EXPORT
int aml_poll(struct aml* self, int timeout)
{
	int next_timeout = aml__get_next_timeout(self, timeout);

	int nfds = aml__poll(self, next_timeout);
	if (nfds < 0)
		return nfds;

	if (nfds == 0) {
		aml__handle_timeout(self);
		return 0;
	}

	return nfds;
}

struct aml_obj* aml__event_dequeue(struct aml* self)
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
	sigset_t sig_old, sig_new;
	sigfillset(&sig_new);

	pthread_sigmask(SIG_BLOCK, &sig_new, &sig_old);

	struct aml_obj* obj;
	while ((obj = aml__event_dequeue(self)) != NULL) {
		aml__handle_event(self, obj);
		aml_unref(obj);
	}

	pthread_sigmask(SIG_SETMASK, &sig_old, NULL);
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
}

EXPORT
void aml_ref(void* obj)
{
	struct aml_obj* self = obj;

	self->ref++;
}

void aml__free(struct aml* self)
{
	self->backend.del_state(self->state);

	while (!LIST_EMPTY(&self->obj_list))
		aml_stop(self, LIST_FIRST(&self->obj_list));

	while (!TAILQ_EMPTY(&self->event_queue)) {
		struct aml_obj* obj = TAILQ_FIRST(&self->event_queue);
		TAILQ_REMOVE(&self->event_queue, obj, event_link);
		aml_unref(obj);
	}

	pthread_mutex_destroy(&self->obj_list_mutex);
	pthread_mutex_destroy(&self->event_queue_mutex);

	free(self);
}

void aml__free_handler(struct aml_handler* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

void aml__free_timer(struct aml_timer* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

void aml__free_signal(struct aml_timer* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

void aml__free_work(struct aml_timer* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

EXPORT
void aml_unref(void* obj)
{
	struct aml_obj* self = obj;

	int ref = --self->ref;
	assert(ref >= 0);
	if (ref > 0)
		return;

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
	default:
		abort();
		break;
	}
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

EXPORT
void aml_emit(struct aml* self, void* ptr, uint32_t revents)
{
	struct aml_obj* obj = ptr;

	if (obj->type == AML_OBJ_HANDLER)
		((struct aml_handler*)ptr)->revents |= revents;

	if (obj->pending++ > 0)
		return;

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
uint32_t aml_get_event_mask(const struct aml_handler* handler)
{
	return handler->event_mask;
}

EXPORT
void aml_set_event_mask(struct aml_handler* handler, uint32_t event_mask)
{
	handler->event_mask = event_mask;

	if (handler->parent)
		aml__mod_fd(handler->parent, handler);
}

EXPORT
uint32_t aml_get_revents(const struct aml_handler* handler)
{
	return handler->revents;
}

EXPORT
int aml_get_fd(const void* ptr)
{
	const struct aml_obj* obj = ptr;

	switch (obj->type) {
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

EXPORT
aml_callback_fn aml_get_work_fn(const struct aml_work* work)
{
	return work->work_fn;
}
