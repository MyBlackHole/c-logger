#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <fcntl.h>

/* 公开合同只链接真实静态库或 DSO；此文件不使用私有布局或包装器。 */
enum record_api { RECORD_LOG, RECORD_SOURCE, RECORD_DEFAULT, RECORD_SYNC };

static FILE *report;
#undef CHECK
#define CHECK(expr)                                                           \
	do {                                                                  \
		if (!(expr)) {                                                \
			fprintf(report ? report : stderr,                     \
				"%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, \
				#expr, errno);                                \
			exit(1);                                              \
		}                                                             \
	} while (0)

static int record(enum record_api api, logger_t *owner, logger_level_t level,
		  const char *fmt, const char *text, int *count)
{
	logger_source_t source = { .module = "record",
				   .file = "source.c",
				   .func = "public_source",
				   .line = 17 };

	switch (api) {
	case RECORD_LOG:
		return logger_log(owner, level, "record", __FILE__, __LINE__,
				  __func__, fmt, text, count);
	case RECORD_SOURCE:
		return logger_log_source(owner, level, source, fmt, text,
					 count);
	case RECORD_DEFAULT:
		return logger_global_write(level, "record", __FILE__, __LINE__,
					   __func__, fmt, text, count);
	case RECORD_SYNC:
		return logger_log_sync_status(owner, level, "record", __FILE__,
					      __LINE__, __func__, fmt, text,
					      count);
	}
	CHECK(0);
	return -EINVAL;
}

static void result(int rc, int error)
{
	CHECK(rc == -error && errno == EDOM);
}

static void cold(void)
{
	logger_t *poison = (logger_t *)(uintptr_t)1;
	int count = -1;

	for (unsigned api = 0; api < 4; ++api) {
		errno = EDOM;
		result(record(api, poison, LOGGER_INFO, "%s%n", "forbidden",
			      &count),
		       ENODEV);
		CHECK(count == -1);
	}
	/* 冷调用不能隐式建域，随后第一次显式初始化仍必须成功。 */
	CHECK(logger_process_init() == 0);
	errno = EDOM;
	result(record(RECORD_DEFAULT, NULL, LOGGER_INFO, "%s%n", "empty",
		      &count),
	       ENODEV);
	CHECK(count == -1);
}

static void expect_line(FILE *file, const char *body)
{
	char line[8192];
	size_t length = strlen(body);

	clearerr(file);
	CHECK(fgets(line, sizeof(line), file) != NULL);
	size_t total = strlen(line);
	CHECK(total > length + 1 && line[total - 1] == '\n');
	CHECK(!memcmp(line + total - length - 1, body, length));
}

static void ready(int asynchronous)
{
	char directory[] = "/tmp/logger-record-api-XXXXXX";
	enter_temp(directory);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.detail = LOGGER_DETAIL_MINIMAL;
	cfg.async_mode = asynchronous;
	cfg.queue_capacity = 8;
	logger_t *owner = NULL;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	FILE *file = fopen("out.log", "rb");
	CHECK(file);
	static const char *const bodies[] = { "instance", "source", "default",
					      "strict" };

	for (unsigned api = 0; api < 4; ++api) {
		int count = -1;

		errno = EDOM;
		if (api != RECORD_DEFAULT) {
			result(record(api, NULL, LOGGER_INFO, "%s%n", "invalid",
				      &count),
			       EINVAL);
			CHECK(count == -1);
		}
		result(record(api, owner, LOGGER_INFO, NULL, "invalid", &count),
		       EINVAL);
		result(record(api, owner, (logger_level_t)-1, "%s%n", "invalid",
			      &count),
		       EINVAL);
		result(record(api, owner, LOGGER_OFF, "%s%n", "invalid",
			      &count),
		       EINVAL);
		result(record(api, owner, LOGGER_DEBUG, "%s%n", "filtered",
			      &count),
		       0);
		CHECK(count == -1);
		result(record(api, owner, LOGGER_INFO, "%s%n", bodies[api],
			      &count),
		       0);
		CHECK(count == (int)strlen(bodies[api]));
		result(logger_flush(owner), 0);
		expect_line(file, bodies[api]);
	}

	char oversized[5000];
	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	int count = -1;
	errno = EDOM;
	result(record(RECORD_SYNC, owner, LOGGER_INFO, "%s%n", oversized,
		      &count),
	       EOVERFLOW);
	CHECK(count == (int)strlen(oversized));
	clearerr(file);
	CHECK(fgetc(file) == EOF && !ferror(file));
	char retained[4096];
	static const char marker[] = " [truncated]";
	memset(retained, 'x', sizeof(retained));
	memcpy(retained + sizeof(retained) - sizeof(marker), marker,
	       sizeof(marker));
	for (unsigned api = 0; api < 3; ++api) {
		errno = EDOM;
		result(record(api, owner, LOGGER_INFO, "%s%n", oversized,
			      &count),
		       0);
		result(logger_flush(owner), 0);
		expect_line(file, retained);
	}
	clearerr(file);
	CHECK(fgetc(file) == EOF && !ferror(file));
	CHECK(fclose(file) == 0);
	errno = EDOM;
	result(logger_default_set(owner, NULL), 0);
	result(logger_destroy(&owner), 0);
	CHECK(owner == NULL);
	leave_temp(directory);
}

static void stderr_error(void)
{
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.async_mode = 0;
	logger_t *owner = NULL;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	int saved = dup(STDERR_FILENO);
	int pipe_fds[2];
	CHECK(saved >= 0 && pipe(pipe_fds) == 0);
	int report_fd = dup(saved);
	CHECK(report_fd >= 0);
	report = fdopen(report_fd, "w");
	CHECK(report);
	CHECK(close(pipe_fds[0]) == 0);
	CHECK(dup2(pipe_fds[1], STDERR_FILENO) == STDERR_FILENO);
	CHECK(close(pipe_fds[1]) == 0);
	for (unsigned api = 0; api < 4; ++api) {
		int count = -1;
		errno = EDOM;
		result(record(api, owner, LOGGER_INFO, "%s%n", "closed-pipe",
			      &count),
		       EPIPE);
		CHECK(count == (int)strlen("closed-pipe"));
	}
	CHECK(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
	CHECK(close(saved) == 0);
	CHECK(fclose(report) == 0);
	report = NULL;
	errno = EDOM;
	result(logger_default_set(owner, NULL), 0);
	result(logger_flush(owner), EPIPE);
	result(logger_destroy(&owner), EPIPE);
	CHECK(owner == NULL);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "cold"))
		cold();
	else if (!strcmp(argv[1], "stderr-error"))
		stderr_error();
	else {
		CHECK(!strcmp(argv[1], "sync") || !strcmp(argv[1], "async"));
		ready(!strcmp(argv[1], "async"));
	}
	printf("RECORD_API_PASS=%s\n", argv[1]);
	return 0;
}
