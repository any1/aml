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

#pragma once

#include <stdint.h>
#include <unistd.h>

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
};

typedef void (*aml_callback_fn)(void* obj);
typedef void (*aml_free_fn)(void*);

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
 * This function behaves like poll(): it will wait for either a timeout (in ms)
 * or a signal.
 *
 * Returns: -1 on timeout or signal; otherwise number of pending events.
 */
int aml_poll(struct aml*, int timeout);

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
int aml_ref(void* obj);

/* Decrement the reference count by one.
 *
 * Returns how many references there are AFTER the call.
 */
int aml_unref(void* obj);

/* Get global object id.
 *
 * This can be used to break reference loops.
 *
 * Returns an id that can be used to access the object using aml_try_ref().
 */
unsigned long long aml_get_id(const void* obj);

/* Try to reference an object with an id returned by aml_get_id().
 *
 * This increments the reference count by one.
 *
 * Returns the aml object if found. Otherwise NULL.
 */
void* aml_try_ref(unsigned long long id);

/* The following calls create event handler objects.
 *
 * An object will have a reference count of 1 upon creation and must be freed
 * using aml_unref().
 */
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
int aml_get_fd(const void* obj);

/* Associate random data with an object.
 *
 * If a free function is defined, it will be called to free the assigned
 * userdata when the object is freed as a result of aml_unref().
 */
void aml_set_userdata(void* obj, void* userdata, aml_free_fn);
void* aml_get_userdata(const void* obj);

void aml_set_event_mask(struct aml_handler* obj, enum aml_event mask);
enum aml_event aml_get_event_mask(const struct aml_handler* obj);

/* Check which events are pending on an fd event handler.
 */
enum aml_event aml_get_revents(const struct aml_handler* obj);

/* Set timeout/period of a timer/ticker
 *
 * Calling this on a started timer/ticker yields undefined behaviour
 */
void aml_set_duration(void* obj, uint32_t value);

/* Start an event handler.
 *
 * This increases the reference count on the handler object.
 *
 * Returns: 0 on success, -1 if the handler is already started.
 */
int aml_start(struct aml*, void* obj);

/* Stop an event handler.
 *
 * This decreases the reference count on a handler object.
 *
 * Returns: 0 on success, -1 if the handler is already stopped.
 */
int aml_stop(struct aml*, void* obj);

/* Get the signal assigned to a signal handler.
 */
int aml_get_signo(const struct aml_signal* sig);
