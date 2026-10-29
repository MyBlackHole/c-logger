#ifndef LOGGER_TIME_H
#define LOGGER_TIME_H

#include <stdint.h>

/*
 * 私有的单调毫秒时刻与毫秒时长；不同结构体阻止隐式单位混用。
 * 目前仅用于 Syslog 重连，不表示墙钟或新增超时接口。
 * C 仍允许显式提取 value 后重建；这不是完整的单位数据流证明。
 */
struct logger_monotonic_ms {
	uint64_t value;
};

struct logger_duration_ms {
	uint32_t value;
};

static inline int logger_monotonic_ms_before(struct logger_monotonic_ms left,
					     struct logger_monotonic_ms right)
{
	return left.value < right.value;
}

static inline int
logger_monotonic_ms_elapsed(struct logger_monotonic_ms now,
			    struct logger_monotonic_ms since,
			    struct logger_duration_ms interval)
{
	/* 先拒绝倒退，差值保持 64 位；不以起点加时长构造可回绕的期限。 */
	return now.value >= since.value &&
	       now.value - since.value >= interval.value;
}

#endif
