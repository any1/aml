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

#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <stdbool.h>
#include <errno.h>

#include "aml.h"
#include "backend.h"
#include "sys/queue.h"

struct posix_state;

typedef void (*fd_op_fn)(struct posix_state*, struct aml_handler*);

struct posix_fd_op {
	struct aml_handler* handler;
	fd_op_fn call;
	TAILQ_ENTRY(posix_fd_op) link;
};

TAILQ_HEAD(posix_fd_op_queue, posix_fd_op);

struct posix_state {
	struct aml* aml;

	struct pollfd* fds;
	struct aml_handler** handlers;

	uint32_t max_fds;
	uint32_t num_fds;

	pthread_t poller_thread;

	int event_pipe_rfd, event_pipe_wfd;

	struct posix_fd_op_queue fd_ops;
	pthread_mutex_t fd_ops_mutex;

	int nfds;
	pthread_mutex_t wait_mutex;
	pthread_cond_t wait_cond;

	bool waiting_for_dispatch;
	pthread_mutex_t dispatch_mutex;
	pthread_cond_t dispatch_cond;
};

struct signal_handler {
	struct posix_state* state;
	struct aml_signal* sig;

	LIST_ENTRY(signal_handler) link;
};

LIST_HEAD(signal_handler_list, signal_handler);

static int posix_spawn_poller(struct posix_state* self);
static void posix_post_dispatch(void* state);
static void posix_interrupt(void* state);

static struct signal_handler_list signal_handlers = LIST_HEAD_INITIALIZER(NULL);

static int posix__enqueue_fd_op(struct posix_state* self, fd_op_fn call,
                                struct aml_handler* handler)
{
	struct posix_fd_op* op = calloc(1, sizeof(*op));
	if (!op)
		return -1;

	aml_ref(handler);

	op->call = call;
	op->handler = handler;

	pthread_mutex_lock(&self->fd_ops_mutex);
	TAILQ_INSERT_TAIL(&self->fd_ops, op, link);
	pthread_mutex_unlock(&self->fd_ops_mutex);

	posix_interrupt(self);

	return 0;
}

static struct posix_fd_op* posix__dequeue_fd_op(struct posix_state* self)
{
	pthread_mutex_lock(&self->fd_ops_mutex);
	struct posix_fd_op* op = TAILQ_FIRST(&self->fd_ops);
	if (op)
		TAILQ_REMOVE(&self->fd_ops, op, link);
	pthread_mutex_unlock(&self->fd_ops_mutex);
	return op;
}

static struct signal_handler* signal_handler_find_by_signo(int signo)
{
	struct signal_handler* handler;

	LIST_FOREACH(handler, &signal_handlers, link)
		if (aml_get_signo(handler->sig) == signo)
			return handler;

	return NULL;
}

static struct signal_handler* signal_handler_find_by_obj(struct aml_signal* obj)
{
	struct signal_handler* handler;

	LIST_FOREACH(handler, &signal_handlers, link)
		if (handler->sig == obj)
			return handler;

	return NULL;
}

static void posix__signal_handler(int signo)
{
	struct signal_handler* handler;

	LIST_FOREACH(handler, &signal_handlers, link)
		if (aml_get_signo(handler->sig) == signo)
			aml_emit(handler->state->aml, handler->sig, 0);
}

static void dont_block(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int posix_init_event_pipe(struct posix_state* self)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;

	dont_block(fds[0]);
	dont_block(fds[1]);

	self->event_pipe_rfd = fds[0];
	self->event_pipe_wfd = fds[1];

	return 0;
}

static void* posix_new_state(struct aml* aml)
{
	struct posix_state* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->aml = aml;
	self->max_fds = 128;
	self->fds = malloc(sizeof(*self->fds) * self->max_fds);
	self->handlers = malloc(sizeof(*self->handlers) * self->max_fds);
	if (!self->fds || !self->handlers) {
		free(self->fds);
		free(self->handlers);
		goto failure;
	}

	TAILQ_INIT(&self->fd_ops);
	pthread_mutex_init(&self->fd_ops_mutex, NULL);

	pthread_mutex_init(&self->wait_mutex, NULL);
	pthread_cond_init(&self->wait_cond, NULL);

	pthread_mutex_init(&self->dispatch_mutex, NULL);
	pthread_cond_init(&self->dispatch_cond, NULL);

	if (posix_init_event_pipe(self) < 0)
		goto pipe_failure;

	if (posix_spawn_poller(self) < 0)
		goto thread_failure;

	return self;

thread_failure:
	close(self->event_pipe_rfd);
	close(self->event_pipe_wfd);
pipe_failure:
	pthread_mutex_destroy(&self->fd_ops_mutex);
failure:
	free(self);
	return NULL;
}

static int posix__find_handler(struct posix_state* self,
                               struct aml_handler* handler)
{
	for (uint32_t i = 0; i < self->num_fds; ++i)
		if (self->handlers[i] == handler)
			return i;

	return -1;
}

static void posix_del_state(void* state)
{
	struct posix_state* self = state;

	posix_post_dispatch(self);

	pthread_cancel(self->poller_thread);
	pthread_join(self->poller_thread, NULL);

	struct posix_fd_op* op;
	while ((op = posix__dequeue_fd_op(self))) {
		aml_unref(op->handler);
		free(op);
	}

	close(self->event_pipe_rfd);
	close(self->event_pipe_wfd);

	pthread_cond_destroy(&self->dispatch_cond);
	pthread_mutex_destroy(&self->dispatch_mutex);
	pthread_cond_destroy(&self->wait_cond);
	pthread_mutex_destroy(&self->wait_mutex);
	pthread_mutex_destroy(&self->fd_ops_mutex);
	free(self->handlers);
	free(self->fds);
	free(self);
}

static int posix_get_fd(const void* state)
{
	const struct posix_state* self = state;
	return self->event_pipe_rfd;
}

static void posix__apply_fd_ops(struct posix_state* self)
{
	while (1) {
		struct posix_fd_op* op = posix__dequeue_fd_op(self);
		if (!op)
			break;

		op->call(self, op->handler);
		aml_unref(op->handler);
		free(op);
	}
}

static enum aml_event posix_poll_events_to_aml_events(uint32_t poll_events)
{
	enum aml_event aml_events = 0;

	if (poll_events & (POLLIN | POLLPRI))
		aml_events |= AML_EVENT_READ;
	if (poll_events & POLLOUT)
		aml_events |= AML_EVENT_READ;

	return aml_events;
}

static int posix_do_poll(struct posix_state* self, int timeout)
{
	int nfds = poll(self->fds, self->num_fds, timeout);
	if (nfds <= 0)
		return nfds;

	for (uint32_t i = 0; i < self->num_fds; ++i)
		if (self->fds[i].revents) {
			struct pollfd* pfd = &self->fds[i];
			struct aml_handler* handler = self->handlers[i];

			assert(pfd->fd == aml_get_fd(handler));
			enum aml_event events =
				posix_poll_events_to_aml_events(pfd->revents);
			aml_emit(self->aml, handler, events);
		}

	return nfds;
}

static void posix_wake_up_main(struct posix_state* self, int nfds)
{
	pthread_mutex_lock(&self->dispatch_mutex);
	self->waiting_for_dispatch = true;
	pthread_mutex_unlock(&self->dispatch_mutex);

	pthread_mutex_lock(&self->wait_mutex);
	self->nfds = nfds;
	pthread_cond_signal(&self->wait_cond);
	pthread_mutex_unlock(&self->wait_mutex);

	pthread_mutex_lock(&self->dispatch_mutex);
	while (self->waiting_for_dispatch)
		pthread_cond_wait(&self->dispatch_cond, &self->dispatch_mutex);
	pthread_mutex_unlock(&self->dispatch_mutex);
}

static void dummy_handler()
{
}

static void* posix_poll_thread(void* state)
{
	struct posix_state* self = state;

	while (1) {
		posix__apply_fd_ops(self);

		int nfds = posix_do_poll(self, -1);
		if (nfds > 0) {
			char one = 1;
			write(self->event_pipe_wfd, &one, sizeof(one));
		}

		if (nfds != 0)
			posix_wake_up_main(self, nfds);
	}

	return NULL;
}

static int posix_spawn_poller(struct posix_state* self)
{
	struct sigaction sa = { .sa_handler = dummy_handler };
	struct sigaction sa_old;
	sigaction(SIGUSR1, &sa, &sa_old);

	return pthread_create(&self->poller_thread, NULL, posix_poll_thread,
	                      self);

	sigaction(SIGUSR1, &sa_old, NULL);
}

static int posix_poll(void* state, int timeout)
{
	struct posix_state* self = state;
	int nfds;

	if (timeout == 0) {
		pthread_mutex_lock(&self->wait_mutex);
		nfds = self->nfds;
		self->nfds = 0;
		pthread_mutex_unlock(&self->wait_mutex);
	} else if (timeout < 0) {
		pthread_mutex_lock(&self->wait_mutex);
		while (self->nfds == 0)
			pthread_cond_wait(&self->wait_cond, &self->wait_mutex);
		nfds = self->nfds;
		self->nfds = 0;
		pthread_mutex_unlock(&self->wait_mutex);
	} else {
		struct timespec ts = { 0 };
		clock_gettime(CLOCK_REALTIME, &ts);
		uint32_t ms = timeout + ts.tv_nsec / 1000000UL;
		ts.tv_sec += ms / 1000UL;
		ts.tv_nsec = (ms % 1000UL) * 1000000UL;

		pthread_mutex_lock(&self->wait_mutex);
		while (self->nfds == 0) {
			int rc = pthread_cond_timedwait(&self->wait_cond,
			                                &self->wait_mutex, &ts);
			if (rc == ETIMEDOUT)
				break;
		}
		nfds = self->nfds;
		self->nfds = 0;
		pthread_mutex_unlock(&self->wait_mutex);
	}

	if (nfds > 0) {
		char dummy[256];
		while (read(self->event_pipe_rfd, dummy, sizeof(dummy)) == sizeof(dummy));
	} else if (nfds < 0) {
		errno = EINTR;
	}

	return nfds;
}

static uint32_t posix_get_event_mask(struct aml_handler* handler)
{
	uint32_t poll_events  = 0;
	enum aml_event aml_events = aml_get_event_mask(handler);

	if (aml_events & AML_EVENT_READ)
		poll_events |= POLLIN | POLLPRI;
	if (aml_events & AML_EVENT_WRITE)
		poll_events |= POLLOUT;

	return poll_events;
}

static void posix_add_fd_op(struct posix_state* self, struct aml_handler* handler)
{
	if (self->num_fds >= self->max_fds) {
		uint32_t new_max = self->max_fds * 2;
		struct pollfd* fds = realloc(self->fds, sizeof(*fds) * new_max);
		struct aml_handler** hds =
			realloc(self->handlers, sizeof(*hds) * new_max);
		assert(fds && hds);

		self->fds = fds;
		self->handlers = hds;
		self->max_fds = new_max;
	}

	struct pollfd* event = &self->fds[self->num_fds];
	event->events = posix_get_event_mask(handler);
	event->revents = 0;
	event->fd = aml_get_fd(handler);

	self->handlers[self->num_fds] = handler;

	self->num_fds++;
}

static void posix_mod_fd_op(struct posix_state* self, struct aml_handler* handler)
{
	int index = posix__find_handler(self, handler);
	if (index < 0)
		return;

	self->fds[index].fd = aml_get_fd(handler);
	self->fds[index].events = posix_get_event_mask(handler);
}

static void posix_del_fd_op(struct posix_state* self, struct aml_handler* handler)
{
	int index = posix__find_handler(self, handler);
	if (index < 0)
		return;

	self->num_fds--;

	self->fds[index] = self->fds[self->num_fds];
	self->handlers[index] = self->handlers[self->num_fds];
}

static int posix_add_fd(void* state, struct aml_handler* handler)
{
	return posix__enqueue_fd_op(state, posix_add_fd_op, handler);
}

static int posix_mod_fd(void* state, struct aml_handler* handler)
{
	return posix__enqueue_fd_op(state, posix_mod_fd_op, handler);
}

static int posix_del_fd(void* state, struct aml_handler* handler)
{
	return posix__enqueue_fd_op(state, posix_del_fd_op, handler);
}

static int posix_add_signal(void* state, struct aml_signal* sig)
{
	int signo = aml_get_signo(sig);

	struct signal_handler* handler = calloc(1, sizeof(*handler));
	if (!handler)
		return -1;

	handler->state = state;
	handler->sig = sig;

	if (!signal_handler_find_by_signo(signo)) {
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, signo);
		pthread_sigmask(SIG_BLOCK, &set, NULL);

		struct sigaction sa = {
			.sa_handler = posix__signal_handler,
		};

		if (sigaction(aml_get_signo(sig), &sa, NULL) < 0)
			goto failure;
	}

	LIST_INSERT_HEAD(&signal_handlers, handler, link);

	return 0;

failure:
	free(handler);
	return -1;
}

static int posix_del_signal(void* state, struct aml_signal* sig)
{
	struct signal_handler* handler = signal_handler_find_by_obj(sig);
	if (!handler)
		return -1;

	LIST_REMOVE(handler, link);

	if (!signal_handler_find_by_signo(aml_get_signo(sig))) {
		struct sigaction sa = {
			.sa_handler = SIG_DFL,
		};

		int signo = aml_get_signo(sig);

		sigaction(signo, &sa, NULL);

		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, signo);
		pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	}

	free(handler);
	return 0;
}

static void posix_post_dispatch(void* state)
{
	struct posix_state* self = state;

	pthread_mutex_lock(&self->dispatch_mutex);
	self->waiting_for_dispatch = false;
	pthread_cond_signal(&self->dispatch_cond);
	pthread_mutex_unlock(&self->dispatch_mutex);
}

static void posix_interrupt(void* state)
{
	struct posix_state* self = state;
	pthread_kill(self->poller_thread, SIGUSR1);
}

const struct aml_backend posix_backend = {
	.new_state = posix_new_state,
	.del_state = posix_del_state,
	.get_fd = posix_get_fd,
	.poll = posix_poll,
	.exit = NULL,
	.add_fd = posix_add_fd,
	.mod_fd = posix_mod_fd,
	.del_fd = posix_del_fd,
	.add_signal = posix_add_signal,
	.del_signal = posix_del_signal,
	.post_dispatch = posix_post_dispatch,
	.interrupt = posix_interrupt,
};
