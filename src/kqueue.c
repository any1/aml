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

#include <sys/event.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>

struct kq_state {
	struct aml* aml;
	int fd;
};

static void* kq_new_state(struct aml* aml)
{
	struct kq_state* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->aml = aml;

	self->fd = kqueue();
	if (self->fd < 0)
		goto kqueue_failure;

	return self;

kqueue_failure:
	free(self);
	return NULL;
}

static void kq_del_state(void* state)
{
	struct kq_state* self = state;
	close(self->fd);
	free(self);
}

static int kq_get_fd(const void* state)
{
	const struct kq_state* self = state;
	return self->fd;
}

static void kq_emit_event(struct kq_state* self, struct kevent* event)
{
	// TODO: Maybe joint read/write into one for fds?
	switch (event->filter) {
	case EVFILT_READ:
		aml_emit(self->aml, event->udata, AML_EVENT_READ);
		break;
	case EVFILT_WRITE:
		aml_emit(self->aml, event->udata, AML_EVENT_WRITE);
		break;
	case EVFILT_SIGNAL:
		aml_emit(self->aml, event->udata, 0);
		break;
	case EVFILT_TIMER:
		assert(event->ident == 0);
		break;
	}
}

static int kq_poll(void* state, int timeout)
{
	struct kq_state* self = state;

	struct timespec ts = {
		.tv_sec = timeout / 1000UL,
		.tv_nsec = (timeout % 1000UL) * 1000000UL,
	};

	struct kevent events[16];
	size_t max_events = sizeof(events) / sizeof(events[0]);

	int nfds = kevent(self->fd, NULL, 0, events, max_events, &ts);
	for (int i = 0; i < nfds; ++i)
		kq_emit_event(self, &events[i]);
		
	return nfds;
}

static int kq_add_fd(void* state, struct aml_handler* handler)
{
	struct kq_state* self = state;
	int fd = aml_get_fd(handler);

	enum aml_event last_mask = (intptr_t)aml_get_backend_data(handler);
	enum aml_event mask = aml_get_event_mask(handler);
	aml_set_backend_data(handler, (void*)(intptr_t)mask);

	struct kevent events[2];
	int n = 0;

	if ((mask ^ last_mask) & AML_EVENT_READ)
		EV_SET(&events[n++], fd, EVFILT_READ,
				mask & AML_EVENT_READ ? EV_ADD : EV_DELETE,
				0, 0, handler);
		
	if ((mask ^ last_mask) & AML_EVENT_WRITE)
		EV_SET(&events[n++], fd, EVFILT_WRITE,
				mask & AML_EVENT_WRITE ? EV_ADD : EV_DELETE,
				0, 0, handler);

	return kevent(self->fd, events, n, NULL, 0, NULL);
}

static int kq_del_fd(void* state, struct aml_handler* handler)
{
	struct kq_state* self = state;
	int fd = aml_get_fd(handler);

	enum aml_event last_mask = (intptr_t)aml_get_backend_data(handler);

	struct kevent events[2];
	int n = 0;

	if (last_mask & AML_EVENT_READ)
		EV_SET(&events[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

	if (last_mask & AML_EVENT_WRITE)
		EV_SET(&events[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

	return kevent(self->fd, events, n, NULL, 0, NULL);
}

static int kq_add_signal(void* state, struct aml_signal* sig)
{
	struct kq_state* self = state;
	int signo = aml_get_signo(sig);

	struct kevent event;
	EV_SET(&event, signo, EVFILT_SIGNAL, EV_ADD, 0, 0, sig);

	int rc = kevent(self->fd, &event, 1, NULL, 0, NULL);

	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, signo);
	pthread_sigmask(SIG_BLOCK, &ss, NULL);

	return rc;
}

static int kq_del_signal(void* state, struct aml_signal* sig)
{
	struct kq_state* self = state;
	int signo = aml_get_signo(sig);

	struct kevent event;
	EV_SET(&event, signo, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);

	// TODO: Restore signal mask

	return kevent(self->fd, &event, 1, NULL, 0, NULL);
}

static int kq_set_deadline(void* state, uint64_t deadline)
{
	struct kq_state* self = state;

	struct kevent event;
	EV_SET(&event, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
			NOTE_USECONDS | NOTE_ABSTIME, deadline, NULL);

	return kevent(self->fd, &event, 1, NULL, 0, NULL);
}

const struct aml_backend implementation = {
	.new_state = kq_new_state,
	.del_state = kq_del_state,
	.clock = CLOCK_REALTIME,
	.get_fd = kq_get_fd,
	.poll = kq_poll,
	.add_fd = kq_add_fd,
	.mod_fd = kq_add_fd, // Same as add_fd
	.del_fd = kq_del_fd,
	.add_signal = kq_add_signal,
	.del_signal = kq_del_signal,
	.set_deadline = kq_set_deadline,
};
