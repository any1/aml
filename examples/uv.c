#include <stdlib.h>
#include <unistd.h>
#include <aml.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <uv.h>
#include <uv/unix.h>

#define container_of(ptr, type, member)                                        \
({                                                                             \
	const __typeof__(((type*)0)->member)* __mptr = (ptr);                  \
	(type*)((char*)__mptr - offsetof(type, member));                       \
})

struct uv_backend_state {
	struct aml* aml;
	uv_loop_t* loop;
	uv_timer_t timer;
	uv_prepare_t prepare;
};

struct uv_backend_poll {
	uv_poll_t uv_poll;
	struct uv_backend_state* state;
	struct aml_handler* handler;
};

struct uv_backend_signal {
	uv_signal_t uv_signal;
	struct uv_backend_state* state;
	struct aml_signal* aml_sig;
};

struct uv_backend_work {
	uv_work_t uv_work;
	struct uv_backend_state* state;
	struct aml_work* aml_work;
};

static void uv_backend_on_timeout()
{
	// Do nothing. This is handled later in prepare
}

static void uv_backend_on_prepare(uv_prepare_t* prepare)
{
	struct uv_backend_state* state =
		container_of(prepare, struct uv_backend_state, prepare);

	aml_dispatch(state->aml);

	int timeout = aml_get_next_timeout(state->aml, -1);
	if (timeout != -1)
		uv_timer_start(&state->timer, uv_backend_on_timeout, timeout, 0);
}

static void* uv_backend_new_state(struct aml* aml)
{
	struct uv_backend_state* state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->aml = aml;
	state->loop = uv_default_loop();

	uv_timer_init(state->loop, &state->timer);

	int timeout = aml_get_next_timeout(aml, -1);
	if (timeout != -1)
		uv_timer_start(&state->timer, uv_backend_on_timeout, timeout, 0);

	uv_prepare_init(state->loop, &state->prepare);
	uv_prepare_start(&state->prepare, uv_backend_on_prepare);

	return state;
}

static void uv_backend_exit(void* state)
{
	struct uv_backend_state* self = state;
	uv_stop(self->loop);
}

static void uv_backend_del_state(void* state)
{
	free(state);
}

static int uv_events_from_poll_events(uint32_t in)
{
	uint32_t out = 0;

	// TODO: Add more
	if (in & POLLIN) out |= UV_READABLE;
	if (in & POLLOUT) out |= UV_WRITABLE;

	return out;
}

static uint32_t uv_events_to_poll_events(int in)
{
	uint32_t out = 0;

	// TODO: Add more
	if (in & UV_WRITABLE) out |= POLLIN;
	if (in & UV_READABLE) out |= POLLOUT;

	return out;
}

static void uv_backend_emit_fd_event(uv_poll_t* uv_poll, int status, int events)
{
	struct uv_backend_poll* backend_poll = (struct uv_backend_poll*)uv_poll;
	uint32_t revents = uv_events_to_poll_events(events);
	aml_emit(backend_poll->state->aml, backend_poll->handler, revents);
}

static int uv_backend_add_fd(void* state, struct aml_handler* handler)
{
	struct uv_backend_state* self = state;

	struct uv_backend_poll* uv_poll = calloc(1, sizeof(*uv_poll));
	if (!uv_poll)
		return -1;

	uv_poll->state = state;
	uv_poll->handler = handler;

	if (uv_poll_init(self->loop, &uv_poll->uv_poll, aml_get_fd(handler)) < 0)
		goto failure;

	int uv_events = uv_events_from_poll_events(aml_get_event_mask(handler));

	if (uv_poll_start(&uv_poll->uv_poll, uv_events, uv_backend_emit_fd_event) < 0)
		goto failure;

	aml_set_backend_data(handler, uv_poll);

	return 0;

failure:
	return -1;
}

static int uv_backend_del_fd(void* state, struct aml_handler* handler)
{
	uv_poll_t* uv_poll = aml_get_backend_data(handler);
	uv_poll_stop(uv_poll);
	free(uv_poll);
	return 0;
}

static void uv_backend_on_signal(uv_signal_t* uv_sig, int signo)
{
	struct uv_backend_signal* backend_sig =
		(struct uv_backend_signal*)uv_sig;
	aml_emit(backend_sig->state->aml, backend_sig->aml_sig, 0);
}

static int uv_backend_add_signal(void* state, struct aml_signal* sig)
{
	struct uv_backend_state* self = state;

	struct uv_backend_signal* uv_sig = calloc(1, sizeof(*uv_sig));
	if (!uv_sig)
		return -1;

	uv_sig->state = state;
	uv_sig->aml_sig = sig;

	if (uv_signal_init(self->loop, &uv_sig->uv_signal) < 0)
		goto failure;

	int signo = aml_get_signo(sig);

	if (uv_signal_start(&uv_sig->uv_signal, uv_backend_on_signal, signo) < 0)
		goto failure;

	aml_set_backend_data(sig, uv_sig);

	return 0;

failure:
	free(uv_sig);
	return -1;
}

static int uv_backend_del_signal(void* state, struct aml_signal* sig)
{
	uv_signal_t* uv_sig = aml_get_backend_data(sig);
	uv_signal_stop(uv_sig);
	free(uv_sig);
	return 0;
}

static void uv_backend_do_work(uv_work_t* req)
{
	struct uv_backend_work* w = (struct uv_backend_work*)req;

	aml_callback_fn cb = aml_get_work_fn(w->aml_work);
	if (cb)
		cb(w->aml_work);
}

static void uv_backend_after_work(uv_work_t* req, int status)
{
	struct uv_backend_work* w = (struct uv_backend_work*)req;
	aml_emit(w->state->aml, w->aml_work, 0);
	aml_stop(w->state->aml, w->aml_work);
}

static int uv_backend_enqueue_work(void* state, struct aml_work* work)
{
	struct uv_backend_state* self = state;

	struct uv_backend_work* w = calloc(1, sizeof(*w));
	if (!w)
		return -1;

	w->state = state;
	w->aml_work = work;

	int rc = uv_queue_work(self->loop, &w->uv_work, uv_backend_do_work,
	                       uv_backend_after_work);
	if (rc < 0)
		goto failure;

	return 0;

failure:
	free(w);
	return -1;
}

static struct aml_backend uv_backend = {
	.new_state = uv_backend_new_state,
	.del_state = uv_backend_del_state,
	.poll = NULL,
	.exit = uv_backend_exit,
	.add_fd = uv_backend_add_fd,
	.mod_fd = NULL,
	.del_fd = uv_backend_del_fd,
	.add_signal = uv_backend_add_signal,
	.del_signal = uv_backend_del_signal,
	.init_thread_pool = NULL,
	.enqueue_work = uv_backend_enqueue_work,
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
	struct aml* aml = aml_new(&uv_backend, sizeof(uv_backend));
	if (!aml)
		return 1;

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

	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	uv_loop_close(uv_default_loop());

	printf("Exiting...\n");

	aml_unref(aml);
	return 0;

failure:
	aml_unref(aml);
	return 1;
}
