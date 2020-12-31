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

struct aml;
struct aml_handler;
struct aml_signal;
struct aml_work;

typedef void (*aml_callback_fn)(void* obj);

enum {
	AML_BACKEND_EDGE_TRIGGERED = 1 << 0,
};

struct aml_backend {
	uint32_t flags;
	uint32_t clock;
	void* (*new_state)(struct aml*);
	void (*del_state)(void* state);
	int (*get_fd)(const void* state);
	int (*poll)(void* state, int timeout);
	void (*exit)(void* state);
	int (*add_fd)(void* state, struct aml_handler*);
	int (*mod_fd)(void* state, struct aml_handler*);
	int (*del_fd)(void* state, struct aml_handler*);
	int (*add_signal)(void* state, struct aml_signal*);
	int (*del_signal)(void* state, struct aml_signal*);
	int (*set_deadline)(void* state, uint64_t deadline);
	void (*post_dispatch)(void* state);
	void (*interrupt)(void* state);
	int (*thread_pool_acquire)(struct aml*, int n_threads);
	void (*thread_pool_release)(struct aml*);
	int (*thread_pool_enqueue)(struct aml*, struct aml_work*);
};

/* These are for setting random data required by the backend implementation.
 *
 * The backend implementation shall NOT use aml_set_userdata() or
 * aml_get_userdata().
 */
void aml_set_backend_data(void* ptr, void* data);
void* aml_get_backend_data(const void* ptr);

void* aml_get_backend_state(const struct aml*);

/* Get the work function pointer assigned to a work object.
 */
aml_callback_fn aml_get_work_fn(const struct aml_work*);

/* revents is only used for fd events. Zero otherwise.
 * This function may be called inside a signal handler
 */
void aml_emit(struct aml* self, void* obj, uint32_t revents);

/* Get time in milliseconds until the next timeout event.
 *
 * If timeout is -1, this returns:
 *  -1 if no event is pending
 *  0 if a timer has already expired
 *  time until next event, otherwise
 *
 * Otherwise, if timeout is less than the time until the next event, timeout is
 * returned, if it is greater, then the time until next event is returned.
 */
int aml_get_next_timeout(struct aml* self, int timeout);
