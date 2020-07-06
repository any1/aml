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

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include <signal.h>

#include "aml.h"
#include "backend.h"
#include "thread-pool.h"
#include "sys/queue.h"

struct default_work {
	unsigned long long aml_id;
	struct aml_work* work;

	TAILQ_ENTRY(default_work) link;
};

TAILQ_HEAD(default_work_queue, default_work);

static struct default_work_queue default_work_queue =
	TAILQ_HEAD_INITIALIZER(default_work_queue);

static atomic_int n_thread_pool_users = 0;

static pthread_t* thread_pool = NULL;
static pthread_mutex_t work_queue_mutex;
static pthread_cond_t work_queue_cond;

static int n_threads = 0;

static int enqueue_work(struct aml* aml, struct aml_work* work, int broadcast);

static void reap_threads(void)
{
	enqueue_work(NULL, NULL, 1);

	for (int i = 0; i < n_threads; ++i)
		pthread_join(thread_pool[i], NULL);

	free(thread_pool);
	thread_pool = NULL;

	pthread_mutex_destroy(&work_queue_mutex);
	pthread_cond_destroy(&work_queue_cond);

	while (!TAILQ_EMPTY(&default_work_queue)) {
		struct default_work* work = TAILQ_FIRST(&default_work_queue);
		TAILQ_REMOVE(&default_work_queue, work, link);
		free(work);
	}
}

static struct default_work* dequeue_work(void)
{
	struct default_work* work;

	pthread_mutex_lock(&work_queue_mutex);

	while ((work = TAILQ_FIRST(&default_work_queue)) == NULL)
		pthread_cond_wait(&work_queue_cond, &work_queue_mutex);

	if (work->work)
		TAILQ_REMOVE(&default_work_queue, work, link);

	pthread_mutex_unlock(&work_queue_mutex);

	return work;
}

static void* worker_fn(void* context)
{
	(void)context;
	sigset_t ss;
	sigfillset(&ss);
	sigdelset(&ss, SIGCHLD);
	pthread_sigmask(SIG_BLOCK, &ss, NULL);

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	while (1) {
		struct default_work* work = dequeue_work();
		assert(work);

		if (!work->work)
			break;

		aml_callback_fn cb = aml_get_work_fn(work->work);
		if (cb)
			cb(work->work);

		struct aml* aml = aml_try_ref(work->aml_id);
		if (aml) {
			aml_emit(aml, work->work, 0);
			aml_stop(aml, work->work);
			aml_interrupt(aml);
			aml_unref(aml);
		}

		free(work);
	}

	return NULL;
}

int thread_pool_acquire_default(struct aml* aml, int n)
{
	(void)aml;

	int rc = 0;

	if (n_threads == 0) {
		pthread_mutex_init(&work_queue_mutex, NULL);
		pthread_cond_init(&work_queue_cond, NULL);
	}

	if (n > n_threads) {
		pthread_t* new_pool =
			realloc(thread_pool, n * sizeof(pthread_t));
		if (!new_pool)
			return -1;

		thread_pool = new_pool;
	}

	int i;
	for (i = n_threads; i < n; ++i) {
		rc = pthread_create(&thread_pool[i], NULL, worker_fn, NULL);
		if (rc < 0)
			break;
	}

	n_threads = i;

	if (rc < 0)
		goto failure;

	++n_thread_pool_users;

	return rc;

failure:
	errno = rc;
	reap_threads();
	return -1;
}

static int enqueue_work(struct aml* aml, struct aml_work* work, int broadcast)
{
	struct default_work* default_work = calloc(1, sizeof(*default_work));
	if (!default_work)
		return -1;

	default_work->work = work;

	if (aml)
		default_work->aml_id = aml_get_id(aml);
	else
		default_work->aml_id = ULLONG_MAX;

	pthread_mutex_lock(&work_queue_mutex);
	TAILQ_INSERT_TAIL(&default_work_queue, default_work, link);

	if (broadcast)
		pthread_cond_broadcast(&work_queue_cond);
	else
		pthread_cond_signal(&work_queue_cond);

	pthread_mutex_unlock(&work_queue_mutex);
	return 0;
}

int thread_pool_enqueue_default(struct aml* aml, struct aml_work* work)
{
	return enqueue_work(aml, work, 0);
}

void thread_pool_release_default(struct aml* aml)
{
	if (--n_thread_pool_users == 0)
		reap_threads();
}
