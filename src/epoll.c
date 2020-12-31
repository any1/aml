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

#include "aml.h"
#include "backend.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <assert.h>

struct epoll_state {
	struct aml* aml;

	int epoll_fd;
	int timer_fd;
};

struct epoll_signal {
	struct epoll_state* state;
	int fd;
	int ref;
};

static void* epoll_new_state(struct aml* aml)
{
	struct epoll_state* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->aml = aml;

	self->epoll_fd = epoll_create(16);
	if (self->epoll_fd < 0)
		goto epoll_failure;

	self->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (self->timer_fd < 0)
		goto timer_fd_failure;

	struct epoll_event event = {
		.events = EPOLLIN,
	};
	if (epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, self->timer_fd, &event) < 0)
		goto timer_add_failure;

	return self;

timer_add_failure:
	close(self->timer_fd);
timer_fd_failure:
	close(self->epoll_fd);
epoll_failure:
	free(self);
	return NULL;
}

static void epoll_del_state(void* state)
{
	struct epoll_state* self = state;
	close(self->timer_fd);
	close(self->epoll_fd);
	free(self);
}

static int epoll_get_fd(const void* state)
{
	const struct epoll_state* self = state;
	return self->epoll_fd;
}

static void epoll_emit_event(struct epoll_state* self,
		struct epoll_event* event)
{
	if (event->data.ptr == NULL) {
		// Must be the timerfd
		uint64_t count = 0;
		(void)read(self->timer_fd, &count, sizeof(count));
		return;
	}

	enum aml_event aml_events = AML_EVENT_NONE;
	if (event->events & (EPOLLIN | EPOLLPRI))
		aml_events |= AML_EVENT_READ;
	if (event->events & EPOLLOUT)
		aml_events |= AML_EVENT_WRITE;

	aml_emit(self->aml, event->data.ptr, aml_events);
}

static int epoll_poll(void* state, int timeout)
{
	struct epoll_state* self = state;
	struct epoll_event events[16];
	size_t max_events = sizeof(events) / sizeof(events[0]);

	int nfds = epoll_wait(self->epoll_fd, events, max_events, timeout);
	for (int i = 0; i < nfds; ++i) 
		epoll_emit_event(self, &events[i]);

	return nfds;
}

static void epoll_event_from_aml_handler(struct epoll_event* event,
		struct aml_handler* handler)
{
	enum aml_event in = aml_get_event_mask(handler);

	event->events = 0;
	if (in & AML_EVENT_READ)
		event->events |= EPOLLIN | EPOLLPRI;
	if (in & AML_EVENT_WRITE)
		event->events |= EPOLLOUT;

	event->data.ptr = handler;
}

static int epoll_add_fd(void* state, struct aml_handler* handler)
{
	struct epoll_state* self = state;
	struct epoll_event event;
	epoll_event_from_aml_handler(&event, handler);
	return epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, aml_get_fd(handler),
			&event);
}

static int epoll_mod_fd(void* state, struct aml_handler* handler)
{
	struct epoll_state* self = state;
	struct epoll_event event;
	epoll_event_from_aml_handler(&event, handler);
	return epoll_ctl(self->epoll_fd, EPOLL_CTL_MOD, aml_get_fd(handler),
			&event);
}

static int epoll_del_fd(void* state, struct aml_handler* handler)
{
	struct epoll_state* self = state;
	// Dummy event to appease valgrind
	struct epoll_event event = { 0 };
	return epoll_ctl(self->epoll_fd, EPOLL_CTL_MOD, aml_get_fd(handler),
			&event);
}

static void epoll_signal_cleanup(void* userdata)
{
	struct epoll_signal* sig = userdata;
	close(sig->fd);
	free(sig);
}

static void epoll_on_signal(void* obj)
{
	struct aml_handler* handler = obj;
	struct epoll_signal* ctx = aml_get_userdata(handler);

	struct signalfd_siginfo fdsi;
	(void)read(ctx->fd, &fdsi, sizeof(fdsi));

	struct aml_signal* sig = aml_try_ref(ctx->ref);
	if (!sig)
		return;

	aml_emit(ctx->state->aml, sig, 0);
	aml_unref(sig);
}

static int epoll_add_signal(void* state, struct aml_signal* sig)
{
	struct epoll_state* self = state;

	struct epoll_signal* ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	int signo = aml_get_signo(sig);

	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, signo);

	ctx->state = self;
	ctx->ref = aml_get_id(sig);

	ctx->fd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);
	if (ctx->fd < 0)
		goto signalfd_failure;
		
	struct aml_handler* handler =
		aml_handler_new(ctx->fd, epoll_on_signal, ctx,
				epoll_signal_cleanup);
	if (!handler)
		goto handler_failure;

	if (aml_start(self->aml, handler) < 0)
		goto start_failure;

	aml_set_backend_data(sig, handler);

	pthread_sigmask(SIG_BLOCK, &ss, NULL);
	return 0;

start_failure:
	aml_unref(handler);
handler_failure:
	close(ctx->fd);
signalfd_failure:
	free(ctx);
	return -1;
}

static int epoll_del_signal(void* state, struct aml_signal* sig)
{
	struct epoll_state* self = state;

	struct aml_handler* handler = aml_get_backend_data(sig);
	assert(handler);

	int rc = aml_stop(self->aml, handler);
	if (rc >= 0)
		aml_unref(handler);

	return rc;
}

static int epoll_set_deadline(void* state, uint64_t deadline)
{
	struct epoll_state* self = state;

	struct itimerspec it = {
		.it_value = {
			.tv_sec = (uint32_t)(deadline / UINT64_C(1000)),
			.tv_nsec = (uint32_t)((deadline % UINT64_C(1000)) *
				UINT64_C(1000000)),
		},
	};

	return timerfd_settime(self->timer_fd, TFD_TIMER_ABSTIME, &it, NULL);
}

const struct aml_backend implementation = {
	.new_state = epoll_new_state,
	.del_state = epoll_del_state,
	.clock = CLOCK_MONOTONIC,
	.get_fd = epoll_get_fd,
	.poll = epoll_poll,
	.add_fd = epoll_add_fd,
	.mod_fd = epoll_mod_fd,
	.del_fd = epoll_del_fd,
	.add_signal = epoll_add_signal,
	.del_signal = epoll_del_signal,
	.set_deadline = epoll_set_deadline,
};
