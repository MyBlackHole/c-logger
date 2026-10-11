#ifndef LOGGER_FAULT_H
#define LOGGER_FAULT_H
#include <stddef.h>
#include <sys/types.h>
enum logger_fault_point {
	LOGGER_FAULT_FILE_WRITE,
	LOGGER_FAULT_FILE_FSYNC,
	LOGGER_FAULT_FILE_RENAME
};
/* 只有特意分离的 logger_test_support 目标包含钩子。
 * 生产宏也会消除调用参数（包括崩溃点字符串）。
 * 不存在可启用生产故障钩子的运行时环境开关。
 */
#if defined(LOGGER_ENABLE_FAULT_INJECTION) && LOGGER_ENABLE_FAULT_INJECTION
int logger_fault_should_fail(enum logger_fault_point);
ssize_t logger_fault_short_write(size_t requested);
void logger_fault_crash_if_requested(const char *point);
#else
#define logger_fault_should_fail(point) (0)
#define logger_fault_short_write(requested) ((ssize_t)(requested))
#define logger_fault_crash_if_requested(point) ((void)0)
#endif
#endif
