#ifndef EXAMPLE_LOGGER_ADAPTER_H
#define EXAMPLE_LOGGER_ADAPTER_H
#include "logger.h"
#include "example_sdk.h"
#include <limits.h>

/* 编译进宿主，不是 SDK。回调 user 是借用的 logger_t *。
 * 普通 logger_log 是尽力而为；用所有者侧指标或刷新获取错误。
 * 它不是审计提交确认。
 */
static inline void example_to_logger(void *user, const example_log_record_t *r)
{
	if (!user || !r || !r->message || r->message_len > INT_MAX)
		return;
	logger_level_t level;
	switch (r->level) {
	case EXAMPLE_LOG_DEBUG:
		level = LOGGER_DEBUG;
		break;
	case EXAMPLE_LOG_INFO:
		level = LOGGER_INFO;
		break;
	case EXAMPLE_LOG_ERROR:
		level = LOGGER_ERROR;
		break;
	default:
		return;
	}
	/* 把消息视为数据。绝不把插件消息用作 printf 格式串。 */
	logger_log((logger_t *)user, level, r->module, r->file, r->line,
		   r->function, "%.*s", (int)r->message_len, r->message);
}
#endif
