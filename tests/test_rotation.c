#define LOG_MODULE "rotation"
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RECORDS 100
#define PAYLOAD "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

static int verify_file(const char *path, unsigned char seen[RECORDS])
{
	FILE *file = fopen(path, "r");
	if (!file)
		return -1;
	char *line = NULL;
	size_t capacity = 0;
	int rc = 0;
	while (getline(&line, &capacity, file) >= 0) {
		char *record = strstr(line, "record=");
		char expected[128];
		unsigned int id;
		if (!record || sscanf(record, "record=%u", &id) != 1 ||
		    id >= RECORDS || seen[id]) {
			rc = -1;
			break;
		}
		snprintf(expected, sizeof(expected), "record=%u " PAYLOAD "\n",
			 id);
		if (strcmp(record, expected)) {
			rc = -1;
			break;
		}
		seen[id] = 1;
	}
	if (ferror(file))
		rc = -1;
	free(line);
	if (fclose(file))
		rc = -1;
	return rc;
}

/* Only this invocation can populate the private directory. Slot order across
 * files is irrelevant; every emitted sequence must survive exactly once. */
static int verify_directory(const char *directory)
{
	DIR *dir = opendir(directory);
	if (!dir)
		return -1;
	unsigned char seen[RECORDS] = { 0 };
	unsigned int active = 0, archives = 0;
	int rc = 0;
	struct dirent *entry;
	errno = 0;
	while ((entry = readdir(dir))) {
		const char *name = entry->d_name;
		size_t length = strlen(name);
		if (!strcmp(name, "rotation.log"))
			active++;
		else if (!strncmp(name, "rotation.", 9) && length > 13 &&
			 !strcmp(name + length - 5, "Z.log"))
			archives++;
		else
			continue; /* Dot entries and the backend's ownership lock. */
		char path[512];
		int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
		if (n < 0 || (size_t)n >= sizeof(path) ||
		    verify_file(path, seen)) {
			rc = -1;
			break;
		}
		errno = 0;
	}
	if (errno)
		rc = -1;
	if (closedir(dir))
		rc = -1;
	if (active != 1 || !archives)
		rc = -1;
	for (unsigned int i = 0; i < RECORDS; i++)
		if (!seen[i])
			rc = -1;
	return rc;
}

static int cleanup_directory(const char *directory)
{
	DIR *dir = opendir(directory);
	if (!dir)
		return -1;
	int rc = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char path[512];
		int n = snprintf(path, sizeof(path), "%s/%s", directory,
				 entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) || unlink(path))
			rc = -1;
	}
	if (closedir(dir))
		rc = -1;
	if (rmdir(directory))
		rc = -1;
	return rc;
}

int main(void)
{
	char directory[] = "rotation-test.XXXXXX";
	if (!mkdtemp(directory))
		return 1;
	char path[128];
	snprintf(path, sizeof(path), "%s/rotation.log", directory);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1024;
	c.rotation.retention_days = 1;
	int rc = 0;
	if (logger_init(&c)) {
		perror("logger_init");
		rc = 1;
		goto out;
	}
	for (int i = 0; i < RECORDS; i++)
		LOG_INFO("record=%d " PAYLOAD, i);
	if (logger_shutdown_status()) {
		perror("logger_shutdown_status");
		rc = 2;
		goto out;
	}
	if (verify_directory(directory)) {
		fprintf(stderr,
			"rotation: missing archive or invalid record set\n");
		rc = 3;
	}
out:
	if (cleanup_directory(directory) && !rc)
		rc = 4;
	return rc;
}
