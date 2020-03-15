#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

#include "aml.h"
#include "sys/queue.h"

enum {
	PIPE_READ_END = 0,
	PIPE_WRITE_END = 1,
};

struct posix_state {
	struct aml* aml;

	struct pollfd* fds;
	struct aml_handler** handlers;

	uint32_t max_fds;
	uint32_t num_fds;

	int self_pipe[2];
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

	TAILQ_ENTRY(posix_work) link;
};

TAILQ_HEAD(posix_work_queue, posix_work);

static int posix__enqueue_work(void* state, struct aml_work* work,
                               int broadcast);
static void posix__reap_threads(void);

static struct signal_handler_list signal_handlers = LIST_HEAD_INITIALIZER(NULL);

static struct posix_work_queue posix_work_queue =
	TAILQ_HEAD_INITIALIZER(posix_work_queue);

static int n_thread_pool_users = 0;
static pthread_t* thread_pool = NULL;
static pthread_mutex_t work_queue_mutex;
static pthread_cond_t work_queue_cond;
static int n_threads = 0;

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

	if (pipe(self->self_pipe) < 0)
		goto failure;

	struct pollfd* pfd = &self->fds[self->num_fds++];
	pfd->events = POLLIN;
	pfd->fd = self->self_pipe[PIPE_READ_END];

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

	if (--n_thread_pool_users == 0)
		posix__reap_threads();

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


	if (self->fds[0].revents) {
		assert(self->fds[0].fd == self->self_pipe[PIPE_READ_END]);

		char dummy[256];
		read(self->self_pipe[PIPE_READ_END], dummy, sizeof(dummy));
	}

	for (uint32_t i = 1; i < self->num_fds; ++i)
		if (self->fds[i].revents) {
			struct pollfd* pfd = &self->fds[i];
			struct aml_handler* handler = self->handlers[i];

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

static void posix__reap_threads(void)
{
	posix__enqueue_work(NULL, NULL, 1);

	for (int i = 0; i < n_threads; ++i)
		pthread_join(thread_pool[i], NULL);

	free(thread_pool);
	thread_pool = NULL;

	pthread_mutex_destroy(&work_queue_mutex);
	pthread_cond_destroy(&work_queue_cond);

	while (!TAILQ_EMPTY(&posix_work_queue)) {
		struct posix_work* work = TAILQ_FIRST(&posix_work_queue);
		TAILQ_REMOVE(&posix_work_queue, work, link);
		free(work);
	}
}

struct posix_work* posix_work_dequeue(void)
{
	struct posix_work* work;

	pthread_mutex_lock(&work_queue_mutex);

	while ((work = TAILQ_FIRST(&posix_work_queue)) == NULL)
		pthread_cond_wait(&work_queue_cond, &work_queue_mutex);

	if (work->work)
		TAILQ_REMOVE(&posix_work_queue, work, link);

	pthread_mutex_unlock(&work_queue_mutex);

	return work;
}

static void posix__interrupt(struct posix_state* state)
{
	char one = 1;
	write(state->self_pipe[PIPE_WRITE_END], &one, sizeof(one));
}

static void* posix_worker_fn(void* context)
{
	(void)context;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	while (1) {
		struct posix_work* work = posix_work_dequeue();
		assert(work);

		if (!work->work)
			break;

		aml_callback_fn cb = aml_get_work_fn(work->work);
		if (cb)
			cb(work->work);

		aml_emit(work->state->aml, work->work, 0);
		aml_stop(work->state->aml, work->work);

		posix__interrupt(work->state);

		free(work);
	}

	return NULL;
}

static int posix_init_thread_pool(void* state, int n)
{
	int rc = 0;

	if (n_threads == 0) {
		pthread_mutex_init(&work_queue_mutex, NULL);
		pthread_cond_init(&work_queue_cond, NULL);
	}

	if (n > n_threads) {
		pthread_t* new_pool = realloc(thread_pool, n * sizeof(pthread_t));
		if (!new_pool)
			return -1;

		thread_pool = new_pool;
	}

	int i;
	for (i = n_threads; i < n; ++i) {
		rc = pthread_create(&thread_pool[i], NULL, posix_worker_fn, NULL);
		if (rc < 0)
			break;
	}

	n_threads = i;

	if (rc < 0) {
		errno = rc;
		posix__reap_threads();
	} else {
		n_thread_pool_users++;
	}

	return rc;
}

static int posix__enqueue_work(void* state, struct aml_work* work,
                               int broadcast)
{
	struct posix_work* posix_work = calloc(1, sizeof(*posix_work));
	if (!posix_work)
		return -1;

	posix_work->state = state;
	posix_work->work = work;

	pthread_mutex_lock(&work_queue_mutex);
	TAILQ_INSERT_TAIL(&posix_work_queue, posix_work, link);

	if (broadcast)
		pthread_cond_broadcast(&work_queue_cond);
	else
		pthread_cond_signal(&work_queue_cond);

	pthread_mutex_unlock(&work_queue_mutex);
	return 0;
}

static int posix_enqueue_work(void* state, struct aml_work* work)
{
	return posix__enqueue_work(state, work, 0);
}

const struct aml_backend posix_backend = {
	.new_state = posix_new_state,
	.del_state = posix_del_state,
	.poll = posix_poll,
	.add_fd = posix_add_fd,
	.mod_fd = posix_mod_fd,
	.del_fd = posix_del_fd,
	.add_signal = posix_add_signal,
	.del_signal = posix_del_signal,
	.init_thread_pool = posix_init_thread_pool,
	.enqueue_work = posix_enqueue_work,
};
