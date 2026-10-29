#ifndef EXAMPLE_LOGGER_ADAPTER_H
#define EXAMPLE_LOGGER_ADAPTER_H
#include "logger.h"
#include "example_sdk.h"
#include <limits.h>

/*
 * 编译进宿主，不是 SDK。回调 user 是借用的 logger_t *。
 * logger_log 返回提交或同步输出状态；此 void 回调没有状态返回通道，
 * 适配器明确丢弃单次结果。所有者侧指标/刷新仍用于观察输出错误，
 * 不能重建已丢弃的单次提交结果。
 */
static inline void example_to_logger(void *user,
				     const struct example_log_record *r)
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
	(void)logger_log((logger_t *)user, level, r->module, r->file, r->line,
			 r->function, "%.*s", (int)r->message_len, r->message);
}
#endif
