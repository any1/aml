#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <poll.h>
#include <fcntl.h>
#include <stdbool.h>

#include "aml.h"
#include "sys/queue.h"

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

	LIST_ENTRY(aml_obj) link;
};

LIST_HEAD(aml_obj_list, aml_obj);

struct aml {
	struct aml_obj obj;

	void* state;
	struct aml_backend backend;

	struct aml_fd_event* revents;
	size_t revents_len;

	int self_pipe[2];

	bool do_exit;

	struct aml_obj_list obj_list;
};

struct aml_handler {
	struct aml_obj obj;

	int fd;
	uint32_t event_mask;
	aml_callback_fn cb;
};

extern struct aml_backend posix_backend;

static int aml__poll(struct aml* self, int timeout)
{
	return self->backend.poll(self->state, &self->revents,
	                          &self->revents_len, timeout);
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

struct aml* aml_new(const struct aml_backend* backend, size_t backend_size)
{
	struct aml* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->obj.type = AML_OBJ_AML;
	self->obj.ref = 1;

	if (backend_size > sizeof(self->backend))
		return NULL;

	if (backend)
		memcpy(&self->backend, backend, backend_size);
	else
		memcpy(&self->backend, &posix_backend, sizeof(self->backend));

	self->state = self->backend.new_state();
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

void aml_interrupt(struct aml* self)
{
	char byte = 0;
	write(self->self_pipe[PIPE_WRITE_END], &byte, 1);
}

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
	aml__obj_ref(self, handler);

	if (aml__add_fd(self, handler->fd, handler->event_mask, handler) < 0)
		goto failure;

	return 0;

failure:
	aml__obj_unref(handler);
	return -1;
}

int aml_start(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__start_handler(self, obj);
	case AML_OBJ_TIMER: return aml__start_timer(self, obj);
	case AML_OBJ_TICKER: return aml__start_ticker(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

int aml__stop_handler(struct aml* self, struct aml_handler* handler)
{
	if (aml__del_fd(self, handler->fd) < 0)
		return -1;

	aml__obj_unref(self);

	return 0;
}

int aml_stop(struct aml* self, void* obj)
{
	struct aml_obj* head = obj;

	switch (head->type) {
	case AML_OBJ_AML: return -1;
	case AML_OBJ_HANDLER: return aml__stop_handler(self, obj);
	case AML_OBJ_TIMER: return aml__stop_timer(self, obj);
	case AML_OBJ_TICKER: return aml__stop_ticker(self, obj);
	case AML_OBJ_UNSPEC: break;
	}

	abort();
	return -1;
}

int aml__get_next_timeout(struct aml* self, int timeout)
{
	// TODO
	return 0;
}

void aml__handle_timeout(struct aml* self)
{
}

void aml__handle_fd_event(struct aml* self, struct aml_fd_event* ev)
{
	if (ev->fd == self->self_pipe[PIPE_READ_END]) {
		char dummy[256];
		read(self->self_pipe[PIPE_READ_END], dummy, sizeof(dummy));
		return;
	}

	struct aml_handler* handler = ev->userdata;

	if (handler->cb)
		handler->cb(handler);
}

/* Might exit earlier than timeout. It's up to the user to check */
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

	/* We must hold a reference to all handler objects that are to be
	 * processed here in case aml_stop() is called on any of them before
	 * they are processed.
	 */
	for (int i = 0; i < nfds; ++i) {
		struct aml_fd_event* ev = &self->revents[i];

		if (ev->userdata)
			aml_ref(ev->userdata);
	}

	for (int i = 0; i < nfds; ++i) {
		struct aml_fd_event* ev = &self->revents[i];

		aml__handle_fd_event(self, ev);

		if (ev->userdata)
			aml_unref(ev->userdata);
	}

	return nfds;
}

int aml_run(struct aml* self)
{
	self->do_exit = false;

	do aml_run_once(self, -1);
	while (!self->do_exit);

	return 0;
}

void aml_exit(struct aml* self)
{
	self->do_exit = true;
	aml_interrupt(self);
}

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
	free(self->revents);
	free(self);
}

void aml__free_handler(struct aml_handler* self)
{
	if (self->obj.free_fn)
		self->obj.free_fn(self->obj.userdata);

	free(self);
}

void aml__free_timer(struct aml_handler* self)
{
	free(self);
}

void aml__free_ticker(struct aml_handler* self)
{
	free(self);
}

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
		aml__free_timer(obj);
		break;
	case AML_OBJ_TICKER:
		aml__free_ticker(obj);
		break;
	default:
		abort();
		break;
	}
}
