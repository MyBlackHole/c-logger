#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <sys/wait.h>

static void result(int rc, int expected)
{
	CHECK(rc == -expected && errno == EDOM);
}

static logger_config_t config(int async)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = async;
	cfg.queue_capacity = 64;
	return cfg;
}

static void ready(int async)
{
	CHECK(logger_process_init() == 0);
	errno = EDOM;
	result(logger_flush(NULL), EINVAL);
	result(logger_reopen(NULL), EINVAL);
	result(logger_default_flush(), ENODEV);
	result(logger_default_reopen(), ENODEV);
	logger_config_t cfg = config(async);
	logger_t *owner = NULL;

	result(logger_create(&cfg, &owner), 0);
	result(logger_default_set(NULL, owner), 0);
	for (int i = 0; i < 32; ++i)
		LOGGER_INFO(owner, "control", "record=%d", i);
	errno = EDOM;
	result(logger_flush(owner), 0);
	result(logger_default_flush(), 0);
	logger_io_metrics_t io;

	CHECK(logger_get_io_metrics(owner, &io) == 0);
	CHECK(io.emitted_records == 32 && io.failed_records == 0);
	result(logger_reopen(owner), 0);
	result(logger_default_reopen(), 0);
	result(logger_destroy(&owner), EBUSY);
	result(logger_default_set(owner, NULL), 0);
	result(logger_default_flush(), ENODEV);
	result(logger_destroy(&owner), 0);
	CHECK(owner == NULL);
	CHECK(file_contains("out.log", "record=0"));
	CHECK(file_contains("out.log", "record=31"));
}

static void reopened_error(void)
{
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = config(0);
	logger_t *owner = NULL;

	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	CHECK(rename("out.log", "original.log") == 0);
	CHECK(mkdir("out.log", 0700) == 0);
	errno = EDOM;
	result(logger_reopen(owner), EISDIR);
	result(logger_default_reopen(), EISDIR);
	result(logger_flush(owner), EISDIR);
	result(logger_default_flush(), EISDIR);
	CHECK(rmdir("out.log") == 0);
	errno = EDOM;
	result(logger_default_reopen(), 0);
	/* 重开成功不清除之前的 I/O 粘滞首错。 */
	result(logger_default_flush(), EISDIR);
	result(logger_default_set(owner, NULL), 0);
	result(logger_destroy(&owner), EISDIR);
	CHECK(owner == NULL);
	CHECK(unlink("original.log") == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char path[] = "/tmp/logger-control-api-XXXXXX";

	enter_temp(path);
	if (!strcmp(argv[1], "cold")) {
		/* 未就绪时必须先拒绝准入，不能读取实例地址。 */
		logger_t *poison = (logger_t *)(uintptr_t)1;

		errno = EDOM;
		result(logger_flush(poison), ENODEV);
		result(logger_reopen(poison), ENODEV);
		result(logger_default_flush(), ENODEV);
		result(logger_default_reopen(), ENODEV);
	} else if (!strcmp(argv[1], "reopen-error")) {
		reopened_error();
	} else {
		CHECK(!strcmp(argv[1], "sync") || !strcmp(argv[1], "async"));
		ready(!strcmp(argv[1], "async"));
	}
	if (!strcmp(argv[1], "cold")) {
		CHECK(access("out.log", F_OK) == -1 && errno == ENOENT);
		CHECK(access("out.log.logger.lock", F_OK) == -1 &&
		      errno == ENOENT);
		CHECK(chdir("/") == 0 && rmdir(path) == 0);
	} else {
		leave_temp(path);
	}
	printf("CONTROL_API_PASS=%s\n", argv[1]);
	return 0;
}
