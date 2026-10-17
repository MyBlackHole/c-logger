#ifndef LOGGER_SYSLOG_H
#define LOGGER_SYSLOG_H
#include "logger.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/un.h>

/* 单一所有者；所有写入与指标访问由 logger_t.emit_mu 串行化。
 * 初始化成功后配置不再变化，也不存在后台重连线程。
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
int logger_syslog_close_status(logger_syslog_t *); /* 0 / -errno；单次消费 fd */
void logger_syslog_close(logger_syslog_t *);
int logger_syslog_write(logger_syslog_t *, logger_level_t, const char *,
			size_t);
#endif
