#ifndef LOGGER_SYSLOG_H
#define LOGGER_SYSLOG_H
#include "logger.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/un.h>

/* 单一 owner；所有 write/metrics 访问由 logger_t.emit_mu 串行化。
 * init 成功后 configuration 不再变化，也不存在后台 reconnect 线程。
 */
typedef struct {
	int fd;
	char ident[128];
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	unsigned facility;
	uint32_t reconnect_interval_ms;
	uint64_t retry_from_ms;
	int retry_clock_pending;
	int connection_error;
	logger_syslog_metrics_t metrics;
} logger_syslog_t;
int logger_syslog_init(logger_syslog_t *, const char *,
		       const logger_syslog_config_t *);
void logger_syslog_close(logger_syslog_t *);
int logger_syslog_write(logger_syslog_t *, logger_level_t, const char *,
			size_t);
#endif
