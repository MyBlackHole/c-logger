#ifndef LOGGER_PROCESS_REJECTION_TEST_H
#define LOGGER_PROCESS_REJECTION_TEST_H

#include "logger.h"
#include "console.h"
#include <errno.h>
#include <string.h>

/* 调用方提供 CHECK。只使用公开接口；普通返回风格仍分别验收。
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
	CHECK(logger_flush_status() == -1 && errno == error);
	CHECK(logger_reopen() == -1 && errno == error);
	LOGGER_INFO(NULL, "init", "forbidden%n", &formatted);
	CHECK(errno == error && formatted == -1);
	logger_global_write(LOGGER_INFO, "init", __FILE__, __LINE__, __func__,
			    "forbidden%n", &formatted);
	CHECK(errno == error && formatted == -1);
	CHECK(logger_log_sync_status(NULL, LOGGER_INFO, "init", __FILE__,
				     __LINE__, __func__, "forbidden%n",
				     &formatted) == -error);
	CHECK(errno == error && formatted == -1);
	CHECK(logger_flush_instance_status(NULL) == -1 && errno == error);
	CHECK(logger_reopen_instance(NULL) == -1 && errno == error);
	logger_set_instance_level(NULL, LOGGER_INFO);
	CHECK(errno == error);
	errno = EDOM;
	CHECK(logger_dropped(NULL) == 0 && errno == error);
	logger_metrics_t metrics;
	memset(&metrics, 0xa5, sizeof(metrics));
	logger_get_metrics(NULL, &metrics);
	CHECK(errno == error && metrics.enqueued == 0);
	logger_io_metrics_t io;
	logger_get_io_metrics(NULL, &io);
	CHECK(errno == error && io.emitted_records == 0);
	logger_diagnostics_t diagnostics;
	logger_get_diagnostics(NULL, &diagnostics);
	CHECK(errno == error && diagnostics.emitted_records == 0);
	logger_file_metrics_t file;
	CHECK(logger_get_file_metrics(NULL, &file) == -1 && errno == error);
	logger_syslog_metrics_t syslog;
	CHECK(logger_get_syslog_metrics(NULL, &syslog) == -1 && errno == error);
	logger_context_set(&(logger_context_t){ .request_id = "forbidden" });
	CHECK(errno == error);
	logger_context_t context = logger_context_get();
	CHECK(errno == error && context.request_id == NULL);
	logger_context_clear();
	CHECK(errno == error);
	console_config_t console = CONSOLE_DEFAULT_CONFIG();
	console_init(&console);
	CHECK(errno == error);
	console_set_color(CONSOLE_COLOR_NEVER);
	CHECK(errno == error);
	CHECK(console_stdout_is_tty() == 0 && errno == error);
	CHECK(console_print("forbidden%n", &formatted) == -1);
	CHECK(errno == error && formatted == -1);
	/* 无运行域的纯辅助函数继续可用，不偷偷初始化。 */
	char masked[32];
	CHECK(logger_redact() != NULL);
	CHECK(logger_mask_secret("secret", masked, sizeof(masked), 0, 0) == 0);
}

#endif
