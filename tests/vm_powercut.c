#include "audit.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_record(const char *name)
{
	audit_event_t event = { .event = name, .actor = "vm-test", .source = "local",
		.resource = "powercut", .operation = "write" };
	return audit_write(&event);
}

static int count_baseline(void)
{
	FILE *file = fopen("powercut.audit.log", "r");
	if (!file)
		return -1;
	char line[8192];
	int count = 0;
	while (fgets(line, sizeof(line), file)) {
		if (strstr(line, "event=\"BASELINE\""))
			++count;
	}
	int failed = ferror(file) || fclose(file) != 0;
	return failed ? -1 : count;
}

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "write") && strcmp(argv[1], "recover")))
		return 2;
	int writer = !strcmp(argv[1], "write");
	if (!writer && count_baseline() != 10) {
		perror("baseline verification");
		return 3;
	}
	audit_config_t config = AUDIT_DEFAULT_CONFIG();
	config.log_dir = ".";
	config.name = "powercut";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&config)) {
		perror("audit_init");
		return 4;
	}
	if (writer) {
		for (int i = 0; i < 10; ++i)
			if (write_record("BASELINE")) {
				perror("baseline write");
				return 5;
			}
		/* The host cuts power as soon as this marker reaches the serial port. */
		puts("POWERCUT_READY");
		fflush(stdout);
		for (;;)
			if (write_record("AFTER_READY")) {
				perror("continued write");
				return 6;
			}
	}
	if (write_record("AFTER_RECOVERY") || audit_shutdown_status() ||
	    audit_verify_file("powercut.audit.log") || count_baseline() != 10) {
		perror("post-recovery verification");
		return 7;
	}
	puts("POWERCUT_RECOVERY_PASS");
	return 0;
}
