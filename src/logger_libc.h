#ifndef LOGGER_LIBC_H
#define LOGGER_LIBC_H
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* 仅串行化本库调用的 libc 格式/时间敏感区间；不声称保护宿主的
 * setlocale/tzset/localtime 或替换分配器。fork 不能从这些回调内部发起。
 */
int logger_libc_vsnprintf(char *out, size_t cap, const char *fmt, va_list args);
int logger_libc_snprintf(char *out, size_t cap, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int logger_libc_vfprintf(FILE *out, const char *fmt, va_list args);
int logger_libc_fprintf(FILE *out, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
struct tm *logger_libc_localtime_r(const time_t *time, struct tm *out);
struct tm *logger_libc_gmtime_r(const time_t *time, struct tm *out);
size_t logger_libc_strftime(char *out, size_t cap, const char *fmt,
			    const struct tm *tm);
int logger_libc_fflush(FILE *out);
int logger_libc_fputc(int c, FILE *out);

int logger_libc_fork_prepare(void);
void logger_libc_fork_parent(void);
void logger_libc_fork_child(void);
int logger_libc_busy(void);
#endif
