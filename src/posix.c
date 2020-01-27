#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>

#include "aml.h"

struct posix_state {
	struct pollfd* fds;
	struct pollfd* fds_copy;
	void** userdata;

	uint32_t max_fds;
	uint32_t num_fds;

	int self_pipe[2];
	int event_pipe[2];

	int nfds;
	int err;

	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static void posix__thread_poll(struct posix_state* self)
{
	pthread_mutex_lock(&self->mutex);
	memcpy(self->fds_copy, self->fds, sizeof(*self->fds_copy) * self->max_fds);
	pthread_mutex_unlock(&self->mutex);

	int nfds = poll(self->fds_copy, self->num_fds, -1);
	// TODO: Copy events within the lock because poll accesses self->fds.

	pthread_mutex_lock(&self->mutex);

	self->nfds = nfds;
	self->err = errno;

	if (nfds != 0) {
		pthread_cond_broadcast(&self->cond);
	}

	pthread_mutex_unlock(&self->mutex);

	if (nfds != 0) {
		char one = 1;
		write(self->wfd, &one, 1);
	}
}

static void* posix_new_state(void)
{
	struct posix_state* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	self->max_fds = 128;
	self->fds = malloc(sizeof(*self->fds) * self->max_fds);
	self->userdata = malloc(sizeof(*self->userdata) * self->max_fds);
	if (!self->fds || !self->userdata) {
		free(self->fds);
		free(self->userdata);
		goto failure;
	}

	return self;

failure:
	free(self);
	return NULL;
}

int posix_get_fd(void* state)
{
	struct posix_state* self = state;

	return self->rfd;
}

static int posix__find_fd(struct posix_state* self, int fd)
{
	for (uint32_t i = 0; i < self->num_fds; ++i)
		if (self->fds[i].fd == fd)
			return i;

	return -1;
}

void posix_del_state(void* state)
{
	struct posix_state* self = state;

	free(self->userdata);
	free(self->fds);
	free(self);
}

uint64_t posix__gettime_ms(void)
{
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

void posix__timespec_from_ms(struct timespec* ts, uint64_t ms)
{
	ts->tv_sec = ms / 1000ULL;
	ts->tv_nsec = (ms % 1000ULL) * 1000000ULL;
}

int posix_poll(void* state, struct aml_fd_event* revents, size_t maxevents,
               int timeout)
{
	struct posix_state* self = state;
	int err = 0;

	char dummy[256];
	while (read(self->rfd, dummy, sizeof(dummy)) > 0);

	uint64_t deadline = posix__gettime_ms() + timeout;
	struct timespec ts;
	posix__timespec_from_ms(&ts, deadline);

	pthread_mutex_lock(&self->mutex);

	if (self->nfds == 0)
		pthread_cond_timedwait(&self->cond, &self->mutex, &ts);

	int n = 0;

	if (self->nfds < 0) {
		n = -1;
		err = self->err;
		goto done;
	}

	for (size_t i = 0; (size_t)n < maxevents && i < (size_t)self->num_fds; ++i)
		if (self->fds[i].revents) {
			struct pollfd* pollfd = &self->fds[i];
			struct aml_fd_event* event = &revents[n++];

			event->fd = pollfd->fd;
			event->event_mask = pollfd->revents;
			event->userdata = self->userdata[i];
		}

done:
	self->nfds = 0;
	pthread_mutex_unlock(&self->mutex);

	errno = err;
	return n;
}

int posix_add_fd(void* state, const struct aml_fd_event* fdev)
{
	int rc = -1;
	struct posix_state* self = state;

	pthread_mutex_lock(&self->mutex);

	if (self->num_fds >= self->max_fds) {
		uint32_t new_max = self->max_fds * 2;
		struct pollfd* fds = realloc(self->fds, sizeof(*fds) * new_max);
		void** uds = realloc(self->userdata, sizeof(*uds) * new_max);
		if (!fds || !fdev->userdata) {
			free(fds);
			free(fdev->userdata);
			goto failure;
		}

		self->fds = fds;
		self->userdata = uds;
		self->max_fds = new_max;
	}

	struct pollfd* event = &self->fds[self->num_fds];
	event->events = fdev->event_mask;
	event->revents = 0;
	event->fd = fdev->fd;

	self->userdata[self->num_fds] = fdev->userdata;

	self->num_fds++;

	rc = 0;
failure:
	pthread_mutex_unlock(&self->mutex);
	posix__signal_thread(self);
	return rc;
}

int posix_mod_fd(void* state, const struct aml_fd_event* fdev)
{
	int rc = -1;
	struct posix_state* self = state;

	pthread_mutex_lock(&self->mutex);

	int index = posix__find_fd(self, fdev->fd);
	if (index < 0)
		goto failure;

	self->fds[index].events = fdev->event_mask;
	self->userdata[index] = fdev->userdata;

	rc = 0;
failure:
	pthread_mutex_unlock(&self->mutex);
	posix__signal_thread(self);
}

int posix_del_fd(void* state, int fd)
{
	int rc = -1;
	struct posix_state* self = state;

	pthread_mutex_lock(&self->mutex);

	int index = posix__find_fd(self, fd);
	if (index < 0)
		goto failure;

	self->num_fds--;

	self->fds[index] = self->fds[self->num_fds];
	self->userdata[index] = self->userdata[self->num_fds];

	rc = 0;
failure:
	pthread_mutex_unlock(&self->mutex);
	posix__signal_thread(self);
}

const struct aml_backend posix_backend = {
	.new_state = posix_new_state,
	.del_state = posix_del_state,
//	.poll = posix_poll,
	.add_fd = posix_add_fd,
	.mod_fd = posix_mod_fd,
	.del_fd = posix_del_fd,
};
