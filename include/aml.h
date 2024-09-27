/*
 * Copyright (c) 2020 - 2024 Andri Yngvason
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

#pragma once

#ifndef AML_UNSTABLE_API
#define AML_UNSTABLE_API 0
#endif

/* Something like this is necessary when changes are made that don't break the
 * build but will cause nasty bugs if ignored.
 */
#if AML_UNSTABLE_API != 1
#error "API has changed! Please, observe the changes and acknowledge by defining AML_UNSTABLE_API as 1 before including aml.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#define aml_ref(obj) _Generic((obj), \
		struct aml*: aml_loop_ref, \
		struct aml_handler*: aml_handler_ref, \
		struct aml_timer*: aml_timer_ref, \
		struct aml_ticker*: aml_ticker_ref, \
		struct aml_signal*: aml_signal_ref, \
		struct aml_work*: aml_work_ref, \
		struct aml_idle*: aml_idle_ref \
		)(obj)

#define aml_unref(obj) _Generic((obj), \
		struct aml*: aml_loop_unref, \
		struct aml_handler*: aml_handler_unref, \
		struct aml_timer*: aml_timer_unref, \
		struct aml_ticker*: aml_ticker_unref, \
		struct aml_signal*: aml_signal_unref, \
		struct aml_work*: aml_work_unref, \
		struct aml_idle*: aml_idle_unref \
		)(obj)

#define aml_set_userdata(aml, obj) _Generic((obj), \
		struct aml*: aml_loop_set_userdata, \
		struct aml_handler*: aml_handler_set_userdata, \
		struct aml_timer*: aml_timer_set_userdata, \
		struct aml_ticker*: aml_ticker_set_userdata, \
		struct aml_signal*: aml_signal_set_userdata, \
		struct aml_work*: aml_work_set_userdata, \
		struct aml_idle*: aml_idle_set_userdata \
		)(aml, obj)

#define aml_get_userdata(obj) _Generic((obj), \
		struct aml*: aml_loop_get_userdata, \
		const struct aml*: aml_loop_get_userdata, \
		struct aml_handler*: aml_handler_get_userdata, \
		const struct aml_handler*: aml_handler_get_userdata, \
		struct aml_timer*: aml_timer_get_userdata, \
		const struct aml_timer*: aml_timer_get_userdata, \
		struct aml_ticker*: aml_ticker_get_userdata, \
		const struct aml_ticker*: aml_ticker_get_userdata, \
		struct aml_signal*: aml_signal_get_userdata, \
		const struct aml_signal*: aml_signal_get_userdata, \
		struct aml_work*: aml_work_get_userdata, \
		const struct aml_work*: aml_work_get_userdata, \
		struct aml_idle*: aml_idle_get_userdata, \
		const struct aml_idle*: aml_idle_get_userdata \
		)(obj)

#define aml_get_fd(obj) _Generic((obj), \
		struct aml*: aml_loop_get_fd, \
		const struct aml*: aml_loop_get_fd, \
		struct aml_handler*: aml_handler_get_fd, \
		const struct aml_handler*: aml_handler_get_fd \
		)(obj)

#define aml_set_duration(obj, duration) _Generic((obj), \
		struct aml_timer*: aml_timer_set_duration, \
		struct aml_ticker*: aml_ticker_set_duration \
		)(obj, duration)

#define aml_start(aml, obj) _Generic((obj), \
		struct aml_handler*: aml_start_handler, \
		struct aml_timer*: aml_start_timer, \
		struct aml_ticker*: aml_start_ticker, \
		struct aml_signal*: aml_start_signal, \
		struct aml_work*: aml_start_work, \
		struct aml_idle*: aml_start_idle \
		)(aml, obj)

#define aml_stop(aml, obj) _Generic((obj), \
		struct aml_handler*: aml_stop_handler, \
		struct aml_timer*: aml_stop_timer, \
		struct aml_ticker*: aml_stop_ticker, \
		struct aml_signal*: aml_stop_signal, \
		struct aml_work*: aml_stop_work, \
		struct aml_idle*: aml_stop_idle \
		)(aml, obj)

#define aml_is_started(aml, obj) _Generic((obj), \
		struct aml_handler*: aml_is_handler_started, \
		struct aml_timer*: aml_is_timer_started, \
		struct aml_ticker*: aml_is_ticker_started, \
		struct aml_signal*: aml_is_signal_started, \
		struct aml_work*: aml_is_work_started, \
		struct aml_idle*: aml_is_idle_started \
		)(aml, obj)

struct aml;
struct aml_handler;
struct aml_timer;
struct aml_ticker;
struct aml_signal;
struct aml_work;
struct aml_idle;

enum aml_event {
	AML_EVENT_NONE = 0,
	AML_EVENT_READ = 1 << 0,
	AML_EVENT_WRITE = 1 << 1,
	AML_EVENT_OOB = 1 << 2,
};

typedef void (*aml_callback_fn)(void* obj);
typedef void (*aml_free_fn)(void*);

extern const char aml_version[];
extern const int aml_unstable_abi_version;

/* Create a new main loop instance */
struct aml* aml_new(void);

/* The backend should supply a minimum of n worker threads in its thread pool.
 *
 * If n == -1, the backend should supply as many workers as there are available
 * CPU cores/threads on the system.
 */
int aml_require_workers(struct aml*, int n);

/* Get/set the default main loop instance */
void aml_set_default(struct aml*);
struct aml* aml_get_default(void);

/* Check if there are pending events. The user should call aml_dispatch()
 * afterwards if there are any pending events.
 *
 * This function behaves like poll(): it will wait for either a timeout (in µs)
 * or a signal. Will block indefinitely if timeout is -1.
 *
 * Returns: -1 on timeout or signal; otherwise number of pending events.
 */
int aml_poll(struct aml*, int64_t timeout);

/* This is a convenience function that calls aml_poll() and aml_dispatch() in
 * a loop until aml_exit() is called.
 */
int aml_run(struct aml*);

/* Instruct the main loop to exit.
 */
void aml_exit(struct aml*);

/* Dispatch pending events */
void aml_dispatch(struct aml* self);

/* Trigger an immediate return from aml_poll().
 */
void aml_interrupt(struct aml*);

/* Increment the reference count by one.
 *
 * Returns how many references there were BEFORE the call.
 */
int aml_loop_ref(struct aml* loop);
int aml_handler_ref(struct aml_handler* obj);
int aml_timer_ref(struct aml_timer* obj);
int aml_ticker_ref(struct aml_ticker* obj);
int aml_signal_ref(struct aml_signal* obj);
int aml_work_ref(struct aml_work* obj);
int aml_idle_ref(struct aml_idle* obj);

/* Decrement the reference count by one.
 *
 * Returns how many references there are AFTER the call.
 */
int aml_loop_unref(struct aml* loop);
int aml_handler_unref(struct aml_handler* obj);
int aml_timer_unref(struct aml_timer* obj);
int aml_ticker_unref(struct aml_ticker* obj);
int aml_signal_unref(struct aml_signal* obj);
int aml_work_unref(struct aml_work* obj);
int aml_idle_unref(struct aml_idle* obj);

/* The following calls create event handler objects.
 *
 * An object will have a reference count of 1 upon creation and must be freed
 * using aml_unref().
 */
struct aml_handler* aml_handler_new(int fd, aml_callback_fn, void* userdata,
                                    aml_free_fn);

struct aml_timer* aml_timer_new(uint64_t timeout, aml_callback_fn,
                                void* userdata, aml_free_fn);

struct aml_ticker* aml_ticker_new(uint64_t period, aml_callback_fn,
                                  void* userdata, aml_free_fn);

struct aml_signal* aml_signal_new(int signo, aml_callback_fn,
                                  void* userdata, aml_free_fn);

struct aml_work* aml_work_new(aml_callback_fn work_fn, aml_callback_fn done_fn,
                              void* userdata, aml_free_fn);

struct aml_idle* aml_idle_new(aml_callback_fn done_fn, void* userdata,
                              aml_free_fn);

/* Get the file descriptor associated with either a handler or the main loop.
 *
 * Calling this on objects of other types is illegal and may cause SIGABRT to
 * be raised.
 *
 * The fd returned from the main loop object can be used in other main loops to
 * monitor events on an aml main loop.
 */
int aml_loop_get_fd(const struct aml* self);
int aml_handler_get_fd(const struct aml_handler* self);

/* Associate random data with an object.
 *
 * If a free function is defined, it will be called to free the assigned
 * userdata when the object is freed as a result of aml_unref().
 */
void aml_loop_set_userdata(struct aml* obj, void* userdata, aml_free_fn);
void aml_handler_set_userdata(struct aml_handler* obj, void* userdata, aml_free_fn);
void aml_timer_set_userdata(struct aml_timer* obj, void* userdata, aml_free_fn);
void aml_ticker_set_userdata(struct aml_ticker* obj, void* userdata, aml_free_fn);
void aml_signal_set_userdata(struct aml_signal* obj, void* userdata, aml_free_fn);
void aml_work_set_userdata(struct aml_work* obj, void* userdata, aml_free_fn);
void aml_idle_set_userdata(struct aml_idle* obj, void* userdata, aml_free_fn);

void* aml_loop_get_userdata(const struct aml* obj);
void* aml_handler_get_userdata(const struct aml_handler* obj);
void* aml_timer_get_userdata(const struct aml_timer* obj);
void* aml_ticker_get_userdata(const struct aml_ticker* obj);
void* aml_signal_get_userdata(const struct aml_signal* obj);
void* aml_work_get_userdata(const struct aml_work* obj);
void* aml_idle_get_userdata(const struct aml_idle* obj);

void aml_set_event_mask(struct aml_handler* obj, enum aml_event mask);
enum aml_event aml_get_event_mask(const struct aml_handler* obj);

/* Check which events are pending on an fd event handler.
 */
enum aml_event aml_get_revents(const struct aml_handler* obj);

/* Set timeout/period of a timer/ticker in µs
 *
 * Calling this on a started timer/ticker yields undefined behaviour
 */
void aml_timer_set_duration(struct aml_timer* self, uint64_t value);
void aml_ticker_set_duration(struct aml_ticker* self, uint64_t value);

/* Start an event handler.
 *
 * This increases the reference count on the handler object.
 *
 * Returns: 0 on success, -1 if the handler is already started.
 */
int aml_start_handler(struct aml*, struct aml_handler*);
int aml_start_timer(struct aml*, struct aml_timer*);
int aml_start_ticker(struct aml*, struct aml_ticker*);
int aml_start_signal(struct aml*, struct aml_signal*);
int aml_start_work(struct aml*, struct aml_work*);
int aml_start_idle(struct aml*, struct aml_idle*);

/* Stop an event handler.
 *
 * This decreases the reference count on a handler object.
 *
 * The callback or done function will not be run after this is called. However,
 * for aml_work, the work function may already be executing and it will be
 * allowed to complete.
 *
 * Returns: 0 on success, -1 if the handler is already stopped.
 */
int aml_stop_handler(struct aml*, struct aml_handler*);
int aml_stop_timer(struct aml*, struct aml_timer*);
int aml_stop_ticker(struct aml*, struct aml_ticker*);
int aml_stop_signal(struct aml*, struct aml_signal*);
int aml_stop_work(struct aml*, struct aml_work*);
int aml_stop_idle(struct aml*, struct aml_idle*);

/* Check if an event handler is started.
 *
 * Returns: true if it has been started, false otherwise.
 */
bool aml_is_handler_started(struct aml*, struct aml_handler* obj);
bool aml_is_timer_started(struct aml*, struct aml_timer* obj);
bool aml_is_ticker_started(struct aml*, struct aml_ticker* obj);
bool aml_is_signal_started(struct aml*, struct aml_signal* obj);
bool aml_is_work_started(struct aml*, struct aml_work* obj);
bool aml_is_idle_started(struct aml*, struct aml_idle* obj);

/* Get the signal assigned to a signal handler.
 */
int aml_get_signo(const struct aml_signal* sig);
