#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <aml.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>
#include <wayland-server.h>

struct wl_backend_state {
	struct aml* aml;
	struct wl_event_loop* loop;
};

struct event_source_data {
	void* aml_obj;
	struct wl_backend_state* state;
	struct wl_event_source* src;
};

static bool do_exit = false;
static struct wl_event_loop* wl_loop = NULL;

static void* wl_backend_new_state(struct aml* aml)
{
	struct wl_backend_state* state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->aml = aml;
	state->loop = wl_loop;

	return state;
}

static void wl_backend_del_state(void* state)
{
	free(state);
}

static void wl_backend_exit()
{
	do_exit = true;
}

static uint32_t wl_backend_events_from_poll_events(uint32_t in)
{
	uint32_t out = 0;

	if (in & POLLIN) out |= WL_EVENT_READABLE;
	if (in & POLLOUT) out |= WL_EVENT_WRITABLE;

	return out;
}

static uint32_t wl_backend_events_to_poll_events(uint32_t in)
{
	uint32_t out = 0;

	if (in & WL_EVENT_READABLE) out |= POLLIN;
	if (in & WL_EVENT_WRITABLE) out |= POLLOUT;

	return out;
}

static int wl_backend_on_fd_event(int fd, uint32_t mask, void* userdata)
{
	struct event_source_data* data = userdata;
	uint32_t revents = wl_backend_events_to_poll_events(mask);
	aml_emit(data->state->aml, data->aml_obj, revents);
	return 0;
}

struct event_source_data*
event_source_data_new(void* obj, struct wl_backend_state* state)
{
	struct event_source_data* data = calloc(1, sizeof(*data));
	if (!data)
		return NULL;

	data->aml_obj = obj;
	data->state = state;

	return data;
}

static int wl_backend_add_fd(void* state, struct aml_handler* handler)
{
	struct wl_event_source* src;
	struct wl_backend_state* self = state;
	int fd = aml_get_fd(handler);
	uint32_t aml_event_mask = aml_get_event_mask(handler);
	uint32_t events = wl_backend_events_from_poll_events(aml_event_mask);

	struct event_source_data* data = event_source_data_new(handler, state);
	if (!data)
		return -1;

	src = wl_event_loop_add_fd(self->loop, fd, events,
	                           wl_backend_on_fd_event, data);
	if (!src)
		goto failure;

	data->src = src;
	aml_set_backend_data(handler, data);

	return 0;

failure:
	free(data);
	return -1;
}

static int wl_backend_mod_fd(void* state, struct aml_handler* handler)
{
	struct event_source_data* data = aml_get_backend_data(handler);
	uint32_t aml_event_mask = aml_get_event_mask(handler);
	uint32_t wl_mask = wl_backend_events_from_poll_events(aml_event_mask);
	wl_event_source_fd_update(data->src, wl_mask);
	return 0;
}

static int wl_backend_del_fd(void* state, struct aml_handler* handler)
{
	struct event_source_data* data = aml_get_backend_data(handler);
	wl_event_source_remove(data->src);
	free(data);
	return 0;
}

static int wl_backend_on_signal(int signo, void* userdata)
{
	struct event_source_data* data = userdata;
	aml_emit(data->state->aml, data->aml_obj, 0);
	return 0;
}

static int wl_backend_add_signal(void* state, struct aml_signal* sig)
{
	struct wl_event_source* src;
	struct wl_backend_state* self = state;
	int signo = aml_get_signo(sig);

	struct event_source_data* data = event_source_data_new(sig, state);
	if (!data)
		return -1;

	src = wl_event_loop_add_signal(self->loop, signo, wl_backend_on_signal,
	                               data);
	if (!src)
		goto failure;

	data->src = src;
	aml_set_backend_data(sig, data);

	return 0;

failure:
	free(data);
	return -1;
}

static int wl_backend_del_signal(void* state, struct aml_signal* sig)
{
	struct event_source_data* data = aml_get_backend_data(sig);
	wl_event_source_remove(data->src);
	free(data);
	return 0;
}

static struct aml_backend wl_backend = {
	.new_state = wl_backend_new_state,
	.del_state = wl_backend_del_state,
	.poll = NULL,
	.exit = wl_backend_exit,
	.add_fd = wl_backend_add_fd,
	.mod_fd = wl_backend_mod_fd,
	.del_fd = wl_backend_del_fd,
	.add_signal = wl_backend_add_signal,
	.del_signal = wl_backend_del_signal,
};

static void on_tick(void* ticker)
{
	int* count_ptr = aml_get_userdata(ticker);

	*count_ptr += 1;

	printf("tick %d!\n", *count_ptr);

	if (*count_ptr >= 10)
		aml_exit(aml_get_default());
}

void on_line(void* handler)
{
	char line[256];
	fscanf(stdin, "%s", line);

	printf("Got line: %s\n", line);

	if (strncmp(line, "exit", sizeof(line)) == 0)
		aml_exit(aml_get_default());
}

static void on_sigint(void* sig)
{
	aml_exit(aml_get_default());
}

int main()
{
	wl_loop = wl_event_loop_create();
	if (!wl_loop)
		return 1;

	struct aml* aml = aml_new(&wl_backend, sizeof(wl_backend));
	if (!aml)
		goto failure;

	aml_set_default(aml);

	int count = 0;

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	if (!sig)
		goto failure;

	aml_start(aml, sig);
	aml_unref(sig);

	struct aml_ticker* ticker = aml_ticker_new(1000, on_tick, &count, NULL);
	if (!ticker)
		goto failure;

	aml_start(aml, ticker);
	aml_unref(ticker);

	struct aml_handler* handler =
		aml_handler_new(fileno(stdin), on_line, NULL, NULL);
	if (!handler)
		goto failure;

	aml_start(aml, handler);
	aml_unref(handler);

	while (!do_exit) {
		aml_dispatch(aml);
		int timeout = aml_get_next_timeout(aml, -1);
		wl_event_loop_dispatch(wl_loop, timeout);
	}

	printf("Exiting...\n");

	aml_unref(aml);
	wl_event_loop_destroy(wl_loop);
	return 0;

failure:
	if (aml) aml_unref(aml);
	wl_event_loop_destroy(wl_loop);
	return 1;
}
