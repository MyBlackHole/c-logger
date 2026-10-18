#ifndef LOGGER_QUEUE_SIZE_H
#define LOGGER_QUEUE_SIZE_H

#include <stddef.h>

/* 只约束队列构造计算中的单位；向分配器和既有字段显式交出裸值。
 * C 同型复制和提取 value 后重建不在此类型防护范围内。
 */
struct logger_queue_records {
	size_t value;
};

struct logger_queue_bytes {
	size_t value;
};

#endif
