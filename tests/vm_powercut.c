#define _GNU_SOURCE
#include "log_durability_support.h"
#include <unistd.h>

static const char *cut_point;
static int armed;
void __real_logger_fault_crash_if_requested(const char *point);
void __wrap_logger_fault_crash_if_requested(const char *point)
{
	if (armed && !strcmp(point, cut_point)) {
		printf("POWERCUT_POINT_%s\n", point);
		fflush(stdout);
		for (;;)
			pause();
	}
	__real_logger_fault_crash_if_requested(point);
}

int main(int argc, char **argv)
{
	if (argc != 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "recover")))
		return 2;
	int writer = !strcmp(argv[1], "write");
	cut_point = argv[2];
	if (!writer && durability_verify())
		return 3;
	logger_config_t c = durability_config(cut_point);
	logger_t *l = logger_create(&c);
	if (!l)
		return 4;
	if (writer) {
		if (durability_baseline(l))
			return 5;
		if (!strcmp(cut_point, "acknowledged")) {
			puts("POWERCUT_POINT_acknowledged");
			fflush(stdout);
			for (;;)
				pause();
		}
		armed = 1;
		for (unsigned i = 0; i < 4096; ++i)
			if (durability_record(l, "CRASH_TARGET", i))
				return 6;
		return 7; /* Requested hook was never reached. */
	}
	int rc = durability_record(l, "AFTER_RESTART", 0);
	if (logger_destroy_status(l) || rc || durability_verify())
		return 8;
	puts("POWERCUT_RECOVERY_PASS");
	return 0;
}
