#define LOG_MODULE "rotation"
#include <errno.h>
#include "regression/default_fixture.h"
static logger_t *default_owner;
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RECORDS 100
#define PAYLOAD "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

static int verify_file(const char *path, unsigned int *next)
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
		    id >= RECORDS || id != *next) {
			rc = -1;
			break;
		}
		snprintf(expected, sizeof(expected), "record=%u " PAYLOAD "\n",
			 id);
		if (strcmp(record, expected)) {
			rc = -1;
			break;
		}
		(*next)++;
	}
	if (ferror(file))
		rc = -1;
	free(line);
	if (fclose(file))
		rc = -1;
	return rc;
}

/*
 * 归档名包含 UTC 时间戳，alphasort 因而给出轮转顺序。
 * 先检查归档，再检查活动文件尾部，要求记录序号连续。
 */
static int select_archive(const struct dirent *entry)
{
	const char *name = entry->d_name;
	size_t length = strlen(name);

	return !strncmp(name, "rotation.", 9) && length > 13 &&
	       !strcmp(name + length - 5, "Z.log");
}

static int verify_directory(const char *directory)
{
	struct dirent **archives = NULL;
	int count = scandir(directory, &archives, select_archive, alphasort);

	if (count < 0)
		return -1;
	unsigned int next = 0;
	int rc = count ? 0 : -1;
	char path[512];

	for (int i = 0; i < count; i++) {
		unsigned int before = next;
		int n = snprintf(path, sizeof(path), "%s/%s", directory,
				 archives[i]->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) ||
		    verify_file(path, &next) || next == before)
			rc = -1;
		free(archives[i]);
	}
	free(archives);
	int n = snprintf(path, sizeof(path), "%s/rotation.log", directory);

	if (n < 0 || (size_t)n >= sizeof(path) || verify_file(path, &next))
		rc = -1;
	if (next != RECORDS)
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
	if (logger_process_init() != 0)
		return 1;
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

	if (fixture_default_create(&c, &default_owner)) {
		fputs("DEFAULT_BIND_FAILED\n", stderr);
		rc = 1;
		goto out;
	}
	for (int i = 0; i < RECORDS; i++)
		LOG_INFO("record=%d " PAYLOAD, i);
	if (fixture_default_destroy(&default_owner)) {
		fputs("DEFAULT_UNBIND_OR_DESTROY_FAILED\n", stderr);
		rc = 2;
		goto out;
	}
	if (verify_directory(directory)) {
		fprintf(stderr, "轮转测试：缺少归档或记录集合不符合要求\n");
		rc = 3;
	}
out:
	if (cleanup_directory(directory) && !rc)
		rc = 4;
	return rc;
}
