#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <poll.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>

#include "aml.h"
#include "sys/queue.h"

#define EXPORT __attribute__((visibility("default")))

#define EVENT_MASK_DEFAULT (POLLIN | POLLPRI)

#define PIPE_READ_END 0
#define PIPE_WRITE_END 1

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

enum aml_obj_type {
	AML_OBJ_UNSPEC = 0,
	AML_OBJ_AML,
	AML_OBJ_HANDLER,
	AML_OBJ_TIMER,
	AML_OBJ_TICKER,
};

struct aml_obj {
	enum aml_obj_type type;
	int ref;
	void* userdata;
	aml_free_fn free_fn;

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
	aml_callback_fn cb;
};

struct aml_timer {
	struct aml_obj obj;

	uint32_t timeout;
	uint64_t deadline;
	aml_callback_fn cb;

	LIST_ENTRY(aml_timer) link;
};

LIST_HEAD(aml_timer_list, aml_timer);

struct aml {
	struct aml_obj obj;

	void* state;
	struct aml_backend backend;

	int self_pipe[2];

	bool do_exit;

	struct aml_obj_list obj_list;
	struct aml_timer_list timer_list;
	struct aml_obj_queue event_queue;
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

static int aml__add_fd(struct aml* self, int fd, uint32_t event_mask, void* ud)
{
	struct aml_fd_event ev = {
		.fd = fd,
		.event_mask = event_mask,
		.userdata = ud,
	};

	return self->backend.add_fd(self->state, &ev);
}

static int aml__del_fd(struct aml* self, int fd)
{
	return self->backend.del_fd(self->state, fd);
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

	if (backend_size > sizeof(self->backend))
		return NULL;

	if (backend)
		memcpy(&self->backend, backend, backend_size);
	else
		memcpy(&self->backend, &posix_backend, sizeof(self->backend));

	self->state = self->backend.new_state(self);
	if (!self->state)
		goto failure;

	if (pipe(self->self_pipe) < 0)
		goto self_pipe_failure;

	if (aml__add_fd(self, self->self_pipe[PIPE_READ_END],
	                EVENT_MASK_DEFAULT, NULL) < 0)
		goto self_pipe_fd_failure;

	aml__dont_block(self->self_pipe[PIPE_READ_END]);
	aml__dont_block(self->self_pipe[PIPE_WRITE_END]);

	return self;

self_pipe_fd_failure:
	close(self->self_pipe[PIPE_WRITE_END]);
	close(self->self_pipe[PIPE_READ_END]);
self_pipe_failure:
	self->backend.del_state(self->state);
failure:
	free(self);
	return NULL;
}

EXPORT
void aml_interrupt(struct aml* self)
{
	char byte = 0;
	write(self->self_pipe[PIPE_WRITE_END], &byte, 1);
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

	self->fd = fd;
	self->event_mask = EVENT_MASK_DEFAULT;
	self->cb = callback;

	return self;
}

EXPORT
struct aml_timer* aml_timer_new(uint32_t timeout, aml_callback_fn callback,
                                void* userdata, aml_free_fn free_fn)
{
	struct aml_timer* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_HANDLER;
	self->obj.ref = 1;
	self->obj.userdata = userdata;
	self->obj.free_fn = free_fn;

	self->timeout = timeout;
	self->cb = callback;

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

void aml__obj_ref(struct aml* self, void* obj)
{
	aml_ref(obj);
	LIST_INSERT_HEAD(&self->obj_list, (struct aml_obj*)obj, link);
}

void aml__obj_unref(void* obj)
{
	LIST_REMOVE((struct aml_obj*)obj, link);
	aml_unref(obj);
}

int aml__start_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__add_fd(self, handler->fd, handler->event_mask, handler) < 0)
		return -1;

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

	aml_interrupt(self);

	return 0;
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
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

int aml__stop_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__del_fd(self, handler->fd) < 0)
		return -1;

	aml__obj_unref(handler);

	return 0;
}

int aml__stop_timer(struct aml* self, struct aml_timer* timer)
{
	if (!aml__is_timer_started(self, timer))
		return -1;

	LIST_REMOVE(timer, link);
	aml__obj_unref(timer);

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

	/* A reference is kept here in case a ticker is stopped inside the
	 * callback. We want the object to live until we're done with it.
	 */
	aml_ref(timer);

	if (timer->cb)
		timer->cb(timer);

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

	aml_unref(timer);
}


void aml__handle_self_pipe() {
	// TODO
	/*
	if (aml__event_is_self_pipe(self, ev)) {
		char dummy[256];
		read(self->self_pipe[PIPE_READ_END], dummy, sizeof(dummy));
		return;
	}
	*/
}

void aml__handle_event(struct aml* self, struct aml_obj* obj)
{
	switch (obj->type) {
	case AML_OBJ_HANDLER:;
		struct aml_handler* handler = (struct aml_handler*)obj;
		if (handler->cb)
			handler->cb(obj);
		handler->revents = 0;
		break;
	default:
		abort();//TODO handle other events here too
		break;
	}

	obj->pending = 0;
}

/* Might exit earlier than timeout. It's up to the user to check */
EXPORT
int aml_run_once(struct aml* self, int timeout)
{
	int next_timeout = aml__get_next_timeout(self, timeout);

	int nfds = aml__poll(self, next_timeout);
	if (nfds < 0)
		return nfds;

	if (nfds == 0) {
		aml__handle_timeout(self);
		return 0;
	}

	while (!TAILQ_EMPTY(&self->event_queue)) {
		struct aml_obj* obj = TAILQ_FIRST(&self->event_queue);

		// TODO: Make a special object for self pipe
		aml__handle_event(self, obj);

		TAILQ_REMOVE(&self->event_queue, obj, event_link);
		aml_unref(obj);
	}

	return nfds;
}

EXPORT
int aml_run(struct aml* self)
{
	self->do_exit = false;

	do aml_run_once(self, -1);
	while (!self->do_exit);

	return 0;
}

EXPORT
void aml_exit(struct aml* self)
{
	self->do_exit = true;
	aml_interrupt(self);
}

EXPORT
void aml_ref(void* obj)
{
	struct aml_obj* self = obj;

	self->ref++;
}

void aml__free(struct aml* self)
{
	while (!LIST_EMPTY(&self->obj_list))
		aml__obj_unref(LIST_FIRST(&self->obj_list));

	close(self->self_pipe[PIPE_WRITE_END]);
	close(self->self_pipe[PIPE_READ_END]);
	self->backend.del_state(self->state);
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

EXPORT
void aml_unref(void* obj)
{
	struct aml_obj* self = obj;

	if (--self->ref > 0)
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

	TAILQ_INSERT_TAIL(&self->event_queue, obj, event_link);
	aml_ref(obj);
}
