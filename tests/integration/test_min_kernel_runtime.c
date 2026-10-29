#define _GNU_SOURCE
#include <stdbool.h>
#include <logger.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

static bool check(bool condition, const char *expression, int line)
{
	if (!condition)
		fprintf(stderr, "CHECK failed: %s line=%d errno=%d\n",
			expression, line, errno);
	return condition;
}

#define CHECK(expr) check((expr), #expr, __LINE__)

static int rotation_check(const char *path)
{
	(void)unlink(path);
	char lock[4096];
	if (!CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", path) > 0))
		return -1;
	(void)unlink(lock);

	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1;
	c.rotation.retention_days = 0;

	logger_t *log = NULL;
	if (!CHECK(logger_create(&c, &log) == 0))
		return -1;
	if (!CHECK(log != NULL))
		return -1;
	if (!CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
					  __FILE__, __LINE__, __func__,
					  "first") == 0))
		return -1;
	if (!CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
					  __FILE__, __LINE__, __func__,
					  "second") == 0))
		return -1;
	logger_file_metrics_t m;
	if (!CHECK(logger_get_file_metrics(log, &m) == 0))
		return -1;
	if (!CHECK(m.rotation_attempts >= 1 && m.rotation_successes >= 1))
		return -1;
	if (!CHECK(m.rotation_failures == 0))
		return -1;
	if (!CHECK(logger_destroy(&log) == 0))
		return -1;
	return 0;
}

/*
 * 在每个客户机文件系统上执行已安装生产库的异步、排空与重开契约测试。
 * 这不声称具备物理断电认证。
 */
static int persistence_check(const char *dir)
{
	char path[4096];
	int n = snprintf(path, sizeof(path), "%s/persistence.log", dir);
	if (!CHECK(n > 0 && (size_t)n < sizeof(path)))
		return -1;
	(void)unlink(path);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 128;
	logger_t *log = NULL;
	if (!CHECK(logger_create(&c, &log) == 0))
		return -1;
	if (!CHECK(log != NULL))
		return -1;
	logger_log(log, LOGGER_INFO, "min-kernel", NULL, 0, NULL,
		   "before-reopen");
	if (!CHECK(logger_flush(log) == 0))
		return -1;
	if (!CHECK(logger_reopen(log) == 0))
		return -1;
	logger_log(log, LOGGER_INFO, "min-kernel", NULL, 0, NULL,
		   "after-reopen");
	if (!CHECK(logger_flush(log) == 0))
		return -1;
	logger_file_metrics_t m;
	if (!CHECK(logger_get_file_metrics(log, &m) == 0))
		return -1;
	if (!CHECK(m.data_sync_attempts >= 2 && m.data_sync_failures == 0))
		return -1;
	if (!CHECK(m.reopen_successes == 1 && m.reopen_failures == 0))
		return -1;
	if (!CHECK(logger_destroy(&log) == 0))
		return -1;

	FILE *file = fopen(path, "r");
	if (!CHECK(file != NULL))
		return -1;
	char text[4096];
	size_t length = fread(text, 1, sizeof(text) - 1, file);
	int read_error = ferror(file);
	int close_error = fclose(file);
	if (!CHECK(!read_error && !close_error))
		return -1;
	text[length] = '\0';
	if (!CHECK(strstr(text, "before-reopen") != NULL))
		return -1;
	if (!CHECK(strstr(text, "after-reopen") != NULL))
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	if (!CHECK(logger_process_init() == 0))
		return -1;
	if (argc != 5)
		return 2;

	unsigned char random[32];
	if (!CHECK(getrandom(random, sizeof(random), 0) ==
		   (ssize_t)sizeof(random)))
		return -1;

	if (!CHECK(rotation_check(argv[1]) == 0))
		return -1;
	if (!CHECK(rotation_check(argv[2]) == 0))
		return -1;
	if (!CHECK(persistence_check(argv[3]) == 0))
		return -1;
	if (!CHECK(persistence_check(argv[4]) == 0))
		return -1;

	puts("MIN_KERNEL_RUNTIME_PROBE_OK");
	return 0;
}
