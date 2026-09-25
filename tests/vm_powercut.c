#define _GNU_SOURCE
#include "audit.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int record(const char *name)
{
	audit_event_t event = { .event = name, .actor = "vm-test", .source = "local",
		.resource = "powercut", .operation = "write" };
	return audit_write(&event);
}

static int count_baseline(void)
{
	DIR *dir = opendir(".");
	if (!dir)
		return -1;
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strstr(entry->d_name, ".log"))
			continue;
		FILE *file = fopen(entry->d_name, "r");
		if (!file) {
			closedir(dir);
			return -1;
		}
		char line[8192];
		while (fgets(line, sizeof(line), file))
			if (strstr(line, "event=\"BASELINE\""))
				++count;
		if (ferror(file) || fclose(file)) {
			closedir(dir);
			return -1;
		}
	}
	return closedir(dir) ? -1 : count;
}

static int archive_filter(const struct dirent *entry)
{
	return !strncmp(entry->d_name, "powercut.audit.", 15) &&
		strstr(entry->d_name, ".log") != NULL;
}

static int verify_chain(int rotation)
{
	if (!rotation)
		return audit_verify_file("powercut.audit.log");
	struct dirent **entries;
	int n = scandir(".", &entries, archive_filter, alphasort);
	if (n < 0)
		return -1;
	char previous[65] = { 0 }, next[65];
	int result = 0;
	for (int i = 0; i < n; ++i) {
		if (!result && audit_verify_file_from(entries[i]->d_name,
				previous[0] ? previous : NULL, next)) {
			perror(entries[i]->d_name);
			result = -1;
		}
		if (!result)
			memcpy(previous, next, sizeof(previous));
		free(entries[i]);
	}
	free(entries);
	if (result)
		return result;
	result = audit_verify_file_from("powercut.audit.log",
		previous[0] ? previous : NULL, next);
	if (result)
		perror("powercut.audit.log");
	return result;
}

int main(int argc, char **argv)
{
	if (argc != 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "recover")))
		return 2;
	int writer = !strcmp(argv[1], "write");
	cut_point = argv[2];
	int rotation = !strncmp(cut_point, "file_after_", 11);
	if (!writer && count_baseline() != 10)
		return 3;
	audit_config_t config = AUDIT_DEFAULT_CONFIG();
	config.log_dir = ".";
	config.name = "powercut";
	config.rotation.mode = rotation ? LOGGER_ROTATE_SIZE : LOGGER_ROTATE_NONE;
	if (rotation)
		config.rotation.max_file_size = 750;
	if (audit_init(&config)) {
		perror("audit_init");
		return 4;
	}
	if (writer) {
		for (int i = 0; i < 10; ++i)
			if (record("BASELINE")) {
				perror("baseline write");
				return 5;
			}
		if (audit_shutdown_status() || audit_init(&config)) {
			perror("baseline checkpoint");
			return 6;
		}
		if (!strcmp(cut_point, "acknowledged")) {
			puts("POWERCUT_POINT_acknowledged");
			fflush(stdout);
			for (;;)
				pause();
		}
		armed = 1;
		for (;;)
			if (record("CRASH_TARGET")) {
				perror("target write or missing hook");
				return 7;
			}
	}
	if (record("AFTER_RECOVERY") || audit_shutdown_status() ||
	    verify_chain(rotation) || count_baseline() != 10) {
		perror("post-recovery verification");
		return 8;
	}
	puts("POWERCUT_RECOVERY_PASS");
	return 0;
}
