#define _GNU_SOURCE
#include "crash_support.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Noreturn void crash_fixture_cut(const char *point)
{
	printf("POWERCUT_POINT_%s\n", point);
	if (fflush(stdout))
		_exit(90);
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	if (argc != 3 || !crash_fixture_point_valid(argv[2]))
		return 2;
	if (!strcmp(argv[1], "write"))
		crash_fixture_write(argv[2]);
	else if (!strcmp(argv[1], "recover")) {
		crash_fixture_recover(argv[2]);
		puts("POWERCUT_RECOVERY_PASS");
		return 0;
	}
	return 2;
}
