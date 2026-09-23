#if !defined(LOGGER_ENABLE_FAULT_INJECTION) || !LOGGER_ENABLE_FAULT_INJECTION
#error "logger_fault.c belongs only to logger_test_support"
#endif
#include "logger_fault.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static _Atomic unsigned long calls[7];
static const char *name(enum logger_fault_point p)
{
	static const char *n[] = { "file_write",  "file_fsync",
				   "file_rename", "file_ftruncate",
				   "state_write", "state_fsync",
				   "state_rename" };
	return p >= 0 && p < 7 ? n[p] : "";
}
int logger_fault_should_fail(enum logger_fault_point p)
{
	if ((unsigned)p >= 7)
		return 0;
	const char *x = getenv("LOGGER_FAULT_POINT");
	if (!x || strcmp(x, name(p)))
		return 0;
	unsigned long n =
		atomic_fetch_add_explicit(&calls[p], 1, memory_order_relaxed) +
		1;
	const char *a = getenv("LOGGER_FAULT_AFTER");
	unsigned long at = a ? strtoul(a, NULL, 10) : 1;
	if (!at)
		at = 1;
	if (n != at)
		return 0;
	const char *e = getenv("LOGGER_FAULT_ERRNO");
	errno = e ? (int)strtol(e, NULL, 10) : EIO;
	return 1;
}
ssize_t logger_fault_short_write(size_t requested)
{
	const char *x = getenv("LOGGER_FAULT_SHORT_WRITE");
	if (!x || !requested)
		return (ssize_t)requested;
	unsigned long n = strtoul(x, NULL, 10);
	if (!n)
		return (ssize_t)requested;
	if (n >= requested)
		n = requested > 1 ? requested - 1 : requested;
	return (ssize_t)n;
}

void logger_fault_crash_if_requested(const char *point)
{
	const char *x = getenv("LOGGER_CRASH_POINT");
	if (x && point && !strcmp(x, point))
		_exit(197);
}
