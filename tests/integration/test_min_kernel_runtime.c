#define _GNU_SOURCE
#include <logger.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "CHECK failed: %s line=%d errno=%d\n", \
				#expr, __LINE__, errno);                       \
			return -1;                                         \
		}                                                          \
	} while (0)

static int rotation_check(const char *path)
{
	(void)unlink(path);
	char lock[4096];
	CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", path) > 0);
	(void)unlink(lock);

	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1;
	c.rotation.retention_days = 0;

	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
				     __FILE__, __LINE__, __func__, "first") == 0);
	CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
				     __FILE__, __LINE__, __func__, "second") == 0);
	logger_file_metrics_t m;
	CHECK(logger_get_file_metrics(log, &m) == 0);
	CHECK(m.rotation_attempts >= 1 && m.rotation_successes >= 1);
	CHECK(m.rotation_failures == 0);
	CHECK(logger_destroy_status(log) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 5)
		return 2;

	CHECK(rotation_check(argv[1]) == 0);
	CHECK(rotation_check(argv[2]) == 0);
	char path[4096];
	int n = snprintf(path, sizeof(path), "%s/runtime-probe.log", argv[3]);
	CHECK(n > 0 && (size_t)n < sizeof(path));
	CHECK(rotation_check(path) == 0);
	n = snprintf(path, sizeof(path), "%s/runtime-probe.log", argv[4]);
	CHECK(n > 0 && (size_t)n < sizeof(path));
	CHECK(rotation_check(path) == 0);

	puts("MIN_KERNEL_RUNTIME_PROBE_OK");
	return 0;
}
