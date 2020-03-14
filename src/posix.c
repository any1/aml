#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#include "aml.h"

struct posix_state {
	struct aml* aml;

	struct pollfd* fds;
	struct aml_handler** handlers;

	uint32_t max_fds;
	uint32_t num_fds;
};

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

void posix_del_state(void* state)
{
	struct posix_state* self = state;

	free(self->handlers);
	free(self->fds);
	free(self);
}

int posix_poll(void* state, int timeout)
{
	struct posix_state* self = state;

	int nfds = poll(self->fds, self->num_fds, timeout);
	if (nfds <= 0)
		return nfds;

	for (uint32_t i = 0; i < self->num_fds; ++i)
		if (self->fds[i].revents) {
			struct pollfd* pfd = &self->fds[i];
			struct aml_handler* handler = self->handlers[i];

			aml_emit(self->aml, handler, pfd->revents);
		}

	return nfds;
}

int posix_add_fd(void* state, struct aml_handler* handler)
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

int posix_mod_fd(void* state, struct aml_handler* handler)
{
	struct posix_state* self = state;

	int index = posix__find_handler(self, handler);
	if (index < 0)
		return -1;

	self->fds[index].fd = aml_get_fd(handler);
	self->fds[index].events = aml_get_event_mask(handler);

	return 0;
}

int posix_del_fd(void* state, struct aml_handler* handler)
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

const struct aml_backend posix_backend = {
	.new_state = posix_new_state,
	.del_state = posix_del_state,
	.poll = posix_poll,
	.add_fd = posix_add_fd,
	.mod_fd = posix_mod_fd,
	.del_fd = posix_del_fd,
};
