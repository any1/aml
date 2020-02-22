#include <stdio.h>
#include <aml.h>

void on_tick(void* ticker)
{
	int* count_ptr = aml_get_userdata(ticker);

	*count_ptr += 1;

	printf("tick %d!\n", *count_ptr);

	if (*count_ptr >= 10)
		aml_exit(aml_get_default());
}

int main()
{
	struct aml* aml = aml_new(NULL, 0);
	if (!aml)
		return 1;

	aml_set_default(aml);

	int count = 0;

	struct aml_ticker* ticker = aml_ticker_new(1000, on_tick, &count, NULL);
	if (!ticker)
		goto ticker_failure;

	aml_start(aml, ticker);
	aml_unref(ticker);

	aml_run(aml);

	aml_unref(aml);
	return 0;

ticker_failure:
	aml_unref(aml);
	return 1;
}
