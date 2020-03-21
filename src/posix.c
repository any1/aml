#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>

#include "aml.h"
#include "sys/queue.h"

struct posix_state {
	struct aml* aml;

	struct pollfd* fds;
	struct aml_handler** handlers;

	uint32_t max_fds;
	uint32_t num_fds;
};

struct signal_handler {
	struct posix_state* state;
	struct aml_signal* sig;

	LIST_ENTRY(signal_handler) link;
};

LIST_HEAD(signal_handler_list, signal_handler);

struct posix_work {
	struct posix_state* state;
	struct aml_work* work;
	int32_t state_id;

	TAILQ_ENTRY(posix_work) link;
};

TAILQ_HEAD(posix_work_queue, posix_work);

static struct signal_handler_list signal_handlers = LIST_HEAD_INITIALIZER(NULL);

struct signal_handler* signal_handler_find_by_signo(int signo)
{
	struct signal_handler* handler;

	LIST_FOREACH(handler, &signal_handlers, link)
		if (aml_get_signo(handler->sig) == signo)
			return handler;

	return NULL;
}

struct signal_handler* signal_handler_find_by_obj(struct aml_signal* obj)
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

	return self;

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

	free(self->handlers);
	free(self->fds);
	free(self);
}

static int posix_poll(void* state, int timeout)
{
	struct posix_state* self = state;

	int nfds = poll(self->fds, self->num_fds, timeout);
	if (nfds <= 0)
		return nfds;

	for (uint32_t i = 0; i < self->num_fds; ++i)
		if (self->fds[i].revents) {
			struct pollfd* pfd = &self->fds[i];
			struct aml_handler* handler = self->handlers[i];

			assert(pfd->fd == aml_get_fd(handler));
			aml_emit(self->aml, handler, pfd->revents);
		}

	return nfds;
}

static int posix_add_fd(void* state, struct aml_handler* handler)
{
	struct posix_state* self = state;

	if (self->num_fds >= self->max_fds) {
		uint32_t new_max = self->max_fds * 2;
		struct pollfd* fds = realloc(self->fds, sizeof(*fds) * new_max);
		struct aml_handler** hds =
			realloc(self->handlers, sizeof(*hds) * new_max);
		if (!fds || !hds) {
			free(fds);
			free(hds);
			return -1;
		}

		self->fds = fds;
		self->handlers = hds;
		self->max_fds = new_max;
	}

	struct pollfd* event = &self->fds[self->num_fds];
	event->events = aml_get_event_mask(handler);
	event->revents = 0;
	event->fd = aml_get_fd(handler);

	self->handlers[self->num_fds] = handler;

	self->num_fds++;

	return 0;
}

static int posix_mod_fd(void* state, struct aml_handler* handler)
{
	struct posix_state* self = state;

	int index = posix__find_handler(self, handler);
	if (index < 0)
		return -1;

	self->fds[index].fd = aml_get_fd(handler);
	self->fds[index].events = aml_get_event_mask(handler);

	return 0;
}

static int posix_del_fd(void* state, struct aml_handler* handler)
{
	struct posix_state* self = state;

	int index = posix__find_handler(self, handler);
	if (index < 0)
		return -1;

	self->num_fds--;

	self->fds[index] = self->fds[self->num_fds];
	self->handlers[index] = self->handlers[self->num_fds];

	return 0;
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

		sigaction(aml_get_signo(sig), &sa, NULL);
	}

	free(handler);
	return 0;
}

const struct aml_backend posix_backend = {
	.new_state = posix_new_state,
	.del_state = posix_del_state,
	.poll = posix_poll,
	.exit = NULL,
	.add_fd = posix_add_fd,
	.mod_fd = posix_mod_fd,
	.del_fd = posix_del_fd,
	.add_signal = posix_add_signal,
	.del_signal = posix_del_signal,
};
