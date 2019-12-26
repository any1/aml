#pragma once

#include <stdint.h>
#include <unistd.h>

struct aml;
struct aml_handler;
struct aml_timer;
struct aml_ticker;

struct aml_backend {
	void* (*new_state)(void);
	void (*del_state)(void* state);
	int (*poll)(void* state, int timeout);
	int (*add_fd)(void* state, int fd, uint32_t event_mask, void* userdata);
	int (*mod_fd)(void* state, int fd, uint32_t event_mask);
	int (*del_fd)(void* state, int fd);
};

typedef void (*aml_callback_fn)(void* obj);
typedef void (*aml_free_fn)(void*);

/* Create a new main loop instance */
struct aml* aml_new(const struct aml_backend* backend, size_t backend_size);

/* Get/set the default main loop instance */
void aml_set_default(struct aml*);
struct aml* aml_get_default(void);

int aml_run_once(struct aml*);

int aml_run(struct aml*);
void aml_exit(struct aml*);

void aml_ref(void* obj);
void aml_unref(void* obj);

struct aml_handler* aml_handler_new(int fd, aml_callback_fn, void* userdata,
                                    aml_free_fn);

struct aml_timer* aml_timer_new(uint32_t timeout, aml_callback_fn,
                                void* userdata, aml_free_fn);

struct aml_ticker* aml_ticker_new(uint32_t period, aml_callback_fn,
                                  void* userdata, aml_free_fn);

int aml_get_fd(const void* obj);

void aml_set_userdata(void* obj, void* userdata);
void* aml_get_userdata(const void* obj);

int aml_start(struct aml*, void* obj);
int aml_stop(struct aml*, void* obj);
