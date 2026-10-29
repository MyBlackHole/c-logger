#ifndef LOGGER_SYSLOG_H
#define LOGGER_SYSLOG_H
#include "logger.h"
#include "logger_fd.h"
#include "logger_time.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/un.h>

/* 单一所有者；所有写入与指标访问由 logger_t.emit_mu 串行化。
 * 初始化成功后配置不再变化，也不存在后台重连线程。
 */
struct logger_syslog {
	struct logger_owned_fd fd; /* 私有唯一所有者；无 fd 时 value=-1 */
	char ident[128];
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	unsigned int facility;
	struct logger_duration_ms reconnect_interval_ms;
	struct logger_monotonic_ms retry_from_ms;
	int retry_clock_pending;
	int connection_error;
	logger_syslog_metrics_t metrics;
};
int logger_syslog_init(struct logger_syslog *s, const char *ident,
		       const logger_syslog_config_t *config);
int logger_syslog_close_status(
	struct logger_syslog *s); /* 0 / -errno；单次消费 fd */
void logger_syslog_close(struct logger_syslog *s);
int logger_syslog_write(struct logger_syslog *s, logger_level_t level,
			const char *line, size_t n);
#endif
