#ifndef LOGGER_PROCESS_REJECTION_TEST_H
#define LOGGER_PROCESS_REJECTION_TEST_H

#include "logger.h"
#include "console.h"
#include <errno.h>
#include <string.h>

/*
 * 调用方提供 CHECK。只使用公开接口；普通返回风格仍分别验收。
 * %n 是未格式化的见证，不能通过仅丢弃最终输出来通过本测试。
 */
static void check_process_rejection(int error)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.async_mode = 0;
	logger_t *owner = NULL;
	int formatted = -1;

	errno = EDOM;
	CHECK(logger_create(&cfg, &owner) == -error);
	CHECK(owner == NULL && errno == EDOM);
	CHECK(logger_destroy(&owner) == -error);
	CHECK(owner == NULL && errno == EDOM);
	/* 准入先于读取配置、拥有者槽和实例字段。 */
	CHECK(logger_create(NULL, NULL) == -error && errno == EDOM);
	CHECK(logger_destroy(NULL) == -error && errno == EDOM);
	CHECK(logger_default_set(NULL, NULL) == -error && errno == EDOM);
	CHECK(logger_default_flush() == -error && errno == EDOM);
	CHECK(logger_default_reopen() == -error && errno == EDOM);
	CHECK(LOGGER_INFO(NULL, "init", "forbidden%n", &formatted) == -error);
	CHECK(errno == EDOM && formatted == -1);
	CHECK(logger_log_source(NULL, LOGGER_INFO, LOGGER_SOURCE(),
				"forbidden%n", &formatted) == -error);
	CHECK(errno == EDOM && formatted == -1);
	CHECK(logger_global_write(LOGGER_INFO, "init", __FILE__, __LINE__,
				  __func__, "forbidden%n",
				  &formatted) == -error);
	CHECK(errno == EDOM && formatted == -1);
	CHECK(logger_log_sync_status(NULL, LOGGER_INFO, "init", __FILE__,
				     __LINE__, __func__, "forbidden%n",
				     &formatted) == -error);
	CHECK(errno == EDOM && formatted == -1);
	CHECK(logger_flush(NULL) == -error && errno == EDOM);
	CHECK(logger_reopen(NULL) == -error && errno == EDOM);
	CHECK(logger_set_instance_level(NULL, LOGGER_INFO) == -error &&
	      errno == EDOM);
	CHECK(logger_set_level(LOGGER_INFO) == -error && errno == EDOM);
	logger_state_t state = LOGGER_STATE_RUNNING;
	uint64_t dropped = UINT64_C(777);

	CHECK(logger_get_state(NULL, &state) == -error && errno == EDOM);
	CHECK(state == LOGGER_STATE_RUNNING);
	CHECK(logger_dropped(NULL, &dropped) == -error && errno == EDOM);
	CHECK(dropped == UINT64_C(777));
	CHECK(logger_global_dropped(&dropped) == -error && errno == EDOM);
	CHECK(dropped == UINT64_C(777));
	logger_metrics_t metrics;
	logger_metrics_t metrics_before;

	memset(&metrics, 0xa5, sizeof(metrics));
	memcpy(&metrics_before, &metrics, sizeof(metrics));
	CHECK(logger_get_metrics(NULL, &metrics) == -error && errno == EDOM);
	CHECK(memcmp(&metrics, &metrics_before, sizeof(metrics)) == 0);
	CHECK(logger_get_global_metrics(&metrics) == -error && errno == EDOM);
	CHECK(memcmp(&metrics, &metrics_before, sizeof(metrics)) == 0);
	logger_io_metrics_t io;
	logger_io_metrics_t io_before;

	memset(&io, 0xa5, sizeof(io));
	memcpy(&io_before, &io, sizeof(io));
	CHECK(logger_get_io_metrics(NULL, &io) == -error && errno == EDOM);
	CHECK(memcmp(&io, &io_before, sizeof(io)) == 0);
	CHECK(logger_get_global_io_metrics(&io) == -error && errno == EDOM);
	CHECK(memcmp(&io, &io_before, sizeof(io)) == 0);
	logger_diagnostics_t diagnostics;
	logger_diagnostics_t diagnostics_before;

	memset(&diagnostics, 0xa5, sizeof(diagnostics));
	memcpy(&diagnostics_before, &diagnostics, sizeof(diagnostics));
	CHECK(logger_get_diagnostics(NULL, &diagnostics) == -error &&
	      errno == EDOM);
	CHECK(memcmp(&diagnostics, &diagnostics_before, sizeof(diagnostics)) ==
	      0);
	CHECK(logger_get_global_diagnostics(&diagnostics) == -error &&
	      errno == EDOM);
	CHECK(memcmp(&diagnostics, &diagnostics_before, sizeof(diagnostics)) ==
	      0);
	logger_file_metrics_t file;
	logger_file_metrics_t file_before;

	memset(&file, 0xa5, sizeof(file));
	memcpy(&file_before, &file, sizeof(file));
	CHECK(logger_get_file_metrics(NULL, &file) == -error && errno == EDOM);
	CHECK(memcmp(&file, &file_before, sizeof(file)) == 0);
	CHECK(logger_get_global_file_metrics(&file) == -error && errno == EDOM);
	CHECK(memcmp(&file, &file_before, sizeof(file)) == 0);
	logger_syslog_metrics_t syslog;
	logger_syslog_metrics_t syslog_before;

	memset(&syslog, 0xa5, sizeof(syslog));
	memcpy(&syslog_before, &syslog, sizeof(syslog));
	CHECK(logger_get_syslog_metrics(NULL, &syslog) == -error &&
	      errno == EDOM);
	CHECK(memcmp(&syslog, &syslog_before, sizeof(syslog)) == 0);
	logger_context_t forbidden = { .request_id = "forbidden" };

	CHECK(logger_context_set(&forbidden) == -error && errno == EDOM);
	logger_context_t context = { .request_id = "sentinel",
				     .session_id = "sentinel",
				     .trace_id = "sentinel" };
	logger_context_t context_before = context;

	CHECK(logger_context_get(&context) == -error && errno == EDOM);
	CHECK(memcmp(&context, &context_before, sizeof(context)) == 0);
	CHECK(logger_context_clear() == -error && errno == EDOM);
	console_config_t console = CONSOLE_DEFAULT_CONFIG();
	int tty = 9;

	errno = EDOM;
	CHECK(console_init(&console) == -error && errno == EDOM);
	CHECK(console_set_color(CONSOLE_COLOR_NEVER) == -error);
	CHECK(console_set_verbosity(CONSOLE_NORMAL) == -error);
	CHECK(console_stdout_is_tty(&tty) == -error && tty == 9);
	CHECK(console_stderr_is_tty(&tty) == -error && tty == 9);
	CHECK(console_print("forbidden%n", &formatted) == -error);
	CHECK(errno == EDOM && formatted == -1);
	/* 无运行域的纯辅助函数继续可用，不偷偷初始化。 */
	char masked[32];

	CHECK(logger_redact() != NULL);
	CHECK(logger_mask_secret("secret", masked, sizeof(masked), 0, 0) == 0);
}

#endif /* LOGGER_PROCESS_REJECTION_TEST_H */
