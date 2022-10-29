#include <stdio.h>
#include <aml.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>

static int do_exit = 0;

static void on_tick(void* ticker)
{
	int* count_ptr = aml_get_userdata(ticker);

	*count_ptr += 1;

	printf("tick %d!\n", *count_ptr);

	if (*count_ptr >= 10)
		aml_exit(aml_get_default());
}

static void on_sigint(void* sig)
{
	do_exit = 1;
}

int main()
{
	struct aml* aml = aml_new();
	if (!aml)
		return 1;

	aml_set_default(aml);

	int fd = aml_get_fd(aml);
	assert(fd >= 0);

	int count = 0;

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	if (!sig)
		goto failure;

	aml_start(aml, sig);
	aml_unref(sig);

	struct aml_ticker* ticker = aml_ticker_new(1000000, on_tick, &count, NULL);
	if (!ticker)
		goto failure;

	aml_start(aml, ticker);
	aml_unref(ticker);

	struct pollfd pollfd = {
		.fd = fd,
		.events = POLLIN,
	};

	while (!do_exit) {
		aml_poll(aml, 0);
		aml_dispatch(aml);

		int nfds = poll(&pollfd, 1, -1);
		if (nfds != 1)
			continue;
	}

	printf("Exiting...\n");

	aml_unref(aml);
	return 0;

failure:
	aml_unref(aml);
	return 1;
}
