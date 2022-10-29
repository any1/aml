#include <stdio.h>
#include <aml.h>
#include <signal.h>

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
	aml_exit(aml_get_default());
}

int main()
{
	struct aml* aml = aml_new();
	if (!aml)
		return 1;

	aml_set_default(aml);

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

	aml_run(aml);

	printf("Exiting...\n");

	aml_unref(aml);
	return 0;

failure:
	aml_unref(aml);
	return 1;
}
