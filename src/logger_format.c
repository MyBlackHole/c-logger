#include "logger_internal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *logger_level_name(logger_level_t level)
{
	static const char *names[] = { "TRACE", "DEBUG", "INFO", "WARN",
				       "ERROR", "FATAL", "OFF" };
	return (unsigned)level <= LOGGER_OFF ? names[level] : "UNKNOWN";
}

const char *logger_basename(const char *path)
{
	const char *slash = path ? strrchr(path, '/') : NULL;
	return slash ? slash + 1 : (path ? path : "?");
}

/* libc printf still formats metadata. cap reserves space for the caller's LF;
 * used is bytes stored, NOT printf's would-have-written count. */
static void append(char *, size_t, size_t *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;
static void append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
	if (*used >= cap - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + *used, cap - *used, fmt, ap);
	va_end(ap);
	if (n > 0) {
		size_t room = cap - *used;
		*used += (size_t)n < room ? (size_t)n : room - 1;
	}
}

size_t logger_format_line(const logger_t *l, const logger_message_t *m,
			  char *out, size_t cap)
{
	if (!cap)
		return 0;
	if (!out || !l || !m) {
		errno = EINVAL;
		return 0;
	}
	out[0] = 0;
	if (cap == 1)
		return 0;
	typedef struct {
		time_t sec;
		int valid;
		char date[32], zone[16];
	} timestamp_cache_t;
	static _Thread_local timestamp_cache_t cache;
	time_t sec = m->ts.tv_sec;
	if (!cache.valid || cache.sec != sec) {
		struct tm tm;
		if (!localtime_r(&sec, &tm) ||
		    !strftime(cache.date, sizeof(cache.date),
			      "%Y-%m-%dT%H:%M:%S", &tm) ||
		    !strftime(cache.zone, sizeof(cache.zone), "%z", &tm)) {
			/* Visible failure, not a made-up timestamp or uninitialized tm. */
			strcpy(cache.date, "time-unavailable");
			cache.zone[0] = 0;
			cache.valid = 0;
		} else {
			cache.sec = sec;
			cache.valid = 1;
		}
	}
	size_t used = 0;
#define ADD(...) append(out, cap - 1, &used, __VA_ARGS__)
	ADD("%s.%06ld%s %-5s", cache.date, m->ts.tv_nsec / 1000L, cache.zone,
	    logger_level_name(m->level));
	logger_detail_t detail = logger_internal_detail(l);
	if (detail >= LOGGER_DETAIL_VERBOSE) {
		if (logger_internal_include_pid(l))
			ADD(" pid=%ld", (long)m->pid);
		if (logger_internal_include_tid(l))
			ADD(" tid=%ld", (long)m->tid);
	}
	if (detail >= LOGGER_DETAIL_NORMAL)
		ADD(" [%s]", m->module ? m->module : "app");
	if (detail >= LOGGER_DETAIL_DEBUG) {
		if (m->request_id[0])
			ADD(" request=%s", m->request_id);
		if (m->session_id[0])
			ADD(" session=%s", m->session_id);
		if (m->trace_id[0])
			ADD(" trace=%s", m->trace_id);
		if (logger_internal_include_source(l))
			ADD(" %s:%d %s()", logger_basename(m->file), m->line,
			    m->func ? m->func : "?");
	}
	ADD(" %s", m->text);
#undef ADD
	out[used++] = '\n';
	out[used] = 0;
	return used;
}
