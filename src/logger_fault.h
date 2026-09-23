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
/* Only the deliberately separate logger_test_support target has hooks.
 * Production macros erase call arguments too (including crash-point strings).
 * There is no runtime environment switch that enables production fault hooks.
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
