#pragma once

#include <stdint.h>
#include <unistd.h>

struct aml;
struct aml_handler;
struct aml_timer;
struct aml_ticker;
struct aml_signal;
struct aml_work;

struct aml_backend {
	void* (*new_state)(struct aml*);
	void (*del_state)(void* state);
	int (*poll)(void* state, int timeout);
	int (*add_fd)(void* state, struct aml_handler*);
	int (*mod_fd)(void* state, struct aml_handler*);
	int (*del_fd)(void* state, struct aml_handler*);
	int (*add_signal)(void* state, struct aml_signal*);
	int (*del_signal)(void* state, struct aml_signal*);
	int (*init_thread_pool)(void* state, int n_threads);
	int (*enqueue_work)(void* state, struct aml_work*);
};

typedef void (*aml_callback_fn)(void* obj);
typedef void (*aml_free_fn)(void*);

/* Create a new main loop instance */
struct aml* aml_new(const struct aml_backend* backend, size_t backend_size);

/* The backend should supply a minimum of n worker threads in its thread pool */
int aml_require_workers(struct aml*, int n);

/* Get/set the default main loop instance */
void aml_set_default(struct aml*);
struct aml* aml_get_default(void);

int aml_poll(struct aml*, int timeout);

int aml_run(struct aml*);
void aml_exit(struct aml*);

/* Dispatch pending events */
void aml_dispatch(struct aml* self);

void aml_interrupt(struct aml*);

void aml_ref(void* obj);
void aml_unref(void* obj);

struct aml_handler* aml_handler_new(int fd, aml_callback_fn, void* userdata,
                                    aml_free_fn);

struct aml_timer* aml_timer_new(uint32_t timeout, aml_callback_fn,
                                void* userdata, aml_free_fn);

struct aml_ticker* aml_ticker_new(uint32_t period, aml_callback_fn,
                                  void* userdata, aml_free_fn);

struct aml_signal* aml_signal_new(int signo, aml_callback_fn,
                                  void* userdata, aml_free_fn);

struct aml_work* aml_work_new(aml_callback_fn work_fn, aml_callback_fn done_fn,
                              void* userdata, aml_free_fn);

int aml_get_fd(const void* obj);

void aml_set_userdata(void* obj, void* userdata, aml_free_fn);
void* aml_get_userdata(const void* obj);

void aml_set_event_mask(struct aml_handler* obj, uint32_t event_mask);
uint32_t aml_get_event_mask(const struct aml_handler* obj);

uint32_t aml_get_revents(const struct aml_handler* obj);

int aml_start(struct aml*, void* obj);
int aml_stop(struct aml*, void* obj);

int aml_get_signo(const struct aml_signal* sig);

aml_callback_fn aml_get_work_fn(const struct aml_work*);

/* revents is only used for fd events. Zero otherwise.
 * This function may be called inside a signal handler
 */
void aml_emit(struct aml* self, void* obj, uint32_t revents);

int aml_get_next_timeout(struct aml* self, int timeout);
