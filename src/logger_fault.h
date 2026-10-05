#ifndef LOGGER_FAULT_H
#define LOGGER_FAULT_H
#include <stddef.h>
#include <sys/types.h>
enum logger_fault_point {
	LOGGER_FAULT_FILE_WRITE,
	LOGGER_FAULT_FILE_FSYNC,
	LOGGER_FAULT_FILE_RENAME,
	LOGGER_FAULT_FILE_FTRUNCATE,
	LOGGER_FAULT_STATE_WRITE,
	LOGGER_FAULT_STATE_FSYNC,
	LOGGER_FAULT_STATE_RENAME
};
/* 只有刻意独立出的 logger_test_support 目标包含故障注入钩子。
 * 生产宏会连同调用参数一起消除（包括崩溃点字符串）。
 * 不存在可在运行时通过环境变量启用生产故障钩子的开关。 */
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
