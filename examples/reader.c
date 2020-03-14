#include <stdio.h>
#include <string.h>
#include <aml.h>
#include <signal.h>

void on_line(void* handler)
{
	char line[256];
	fscanf(stdin, "%s", line);
	
	printf("Got line: %s\n", line);

	if (strncmp(line, "exit", sizeof(line)) == 0)
		aml_exit(aml_get_default());
}

void on_sigint(void* sig)
{
	aml_exit(aml_get_default());
}

int main()
{
	struct aml* aml = aml_new(NULL, 0);
	if (!aml)
		return 1;

	aml_set_default(aml);

	struct aml_signal* sig = aml_signal_new(SIGINT, on_sigint, NULL, NULL);
	if (!sig)
		goto failure;

	aml_start(aml, sig);
	aml_unref(sig);

	struct aml_handler* handler =
		aml_handler_new(fileno(stdin), on_line, NULL, NULL);
	if (!handler)
		goto failure;

	aml_start(aml, handler);
	aml_unref(handler);

	aml_run(aml);

	printf("Exiting...\n");

	aml_unref(aml);
	return 0;

failure:
	aml_unref(aml);
	return 1;
}
