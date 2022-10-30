/*
 * Copyright (c) 2020 - 2022 Andri Yngvason
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
int aml_ref(void* obj);

/* Decrement the reference count by one.
 *
 * Returns how many references there are AFTER the call.
 */
int aml_unref(void* obj);

/* Create a new weak reference to the object.
 *
 * The reference object must be deleted using aml_weak_ref_del().
 */
struct aml_weak_ref* aml_weak_ref_new(void* obj);

/* Delete a weak reference created by aml_weak_ref_new().
 */
void aml_weak_ref_del(struct aml_weak_ref* self);

/* Try to get a new strong reference from a weak reference object.
 *
 * If the weak reference is still valid, the reference count on the returned
 * aml object will be increased by one. Otherwise NULL is returned.
 */
void* aml_weak_ref_read(struct aml_weak_ref* self);

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

/* Set timeout/period of a timer/ticker in µs
 *
 * Calling this on a started timer/ticker yields undefined behaviour
 */
void aml_set_duration(void* obj, uint64_t value);

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
 * The callback or done function will not be run after this is called. However,
 * for aml_work, the work function may already be executing and it will be
 * allowed to complete.
 *
 * Returns: 0 on success, -1 if the handler is already stopped.
 */
int aml_stop(struct aml*, void* obj);

/* Check if an event handler is started.
 *
 * Returns: true if it has been started, false otherwise.
 */
bool aml_is_started(struct aml*, void* obj);

/* Get the signal assigned to a signal handler.
 */
int aml_get_signo(const struct aml_signal* sig);
