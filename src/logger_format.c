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

/* cap reserves space for LF; used counts stored, not would-have-written bytes.
 * Ordinary output marks truncation; acknowledged output rejects it. */
static int append(char *out, size_t cap, size_t *used, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;
static int append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
	if (*used >= cap - 1)
		return -EOVERFLOW;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + *used, cap - *used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -EILSEQ;
	size_t room = cap - *used;
	if ((size_t)n >= room) {
		*used += room - 1;
		return -EOVERFLOW;
	}
	*used += (size_t)n;
	return 0;
}

struct timestamp_cache {
	time_t sec;
	int valid;
	char date[32];
	char zone[16];
};

static _Thread_local struct timestamp_cache timestamp;

/* Validate cached bytes too: an ordinary call can populate this same cache.
 * strftime success alone does not imply the strict four-digit-year timestamp grammar. */
static int canonical_time(const struct timestamp_cache *cache)
{
	const char *d = cache->date;
	const char *z = cache->zone;
	static const unsigned days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	if (!cache->valid || strlen(d) != 19 || strlen(z) != 5)
		return 0;
	for (unsigned i = 0; i < 19; ++i) {
		char delimiter = i == 4 || i == 7 ? '-' :
				 i == 10 ? 'T' : i == 13 || i == 16 ? ':' : 0;
		if (delimiter ? d[i] != delimiter : d[i] < '0' || d[i] > '9')
			return 0;
	}
	if (z[0] != '+' && z[0] != '-')
		return 0;
	for (unsigned i = 1; i < 5; ++i)
		if (z[i] < '0' || z[i] > '9')
			return 0;

	unsigned year = (unsigned)(d[0] - '0') * 1000u +
			(unsigned)(d[1] - '0') * 100u +
			(unsigned)(d[2] - '0') * 10u + (unsigned)(d[3] - '0');
	unsigned month = (unsigned)(d[5] - '0') * 10u + (unsigned)(d[6] - '0');
	unsigned day = (unsigned)(d[8] - '0') * 10u + (unsigned)(d[9] - '0');
	unsigned hour = (unsigned)(d[11] - '0') * 10u + (unsigned)(d[12] - '0');
	unsigned minute = (unsigned)(d[14] - '0') * 10u + (unsigned)(d[15] - '0');
	unsigned second = (unsigned)(d[17] - '0') * 10u + (unsigned)(d[18] - '0');
	unsigned zone_hour = (unsigned)(z[1] - '0') * 10u + (unsigned)(z[2] - '0');
	unsigned zone_minute = (unsigned)(z[3] - '0') * 10u + (unsigned)(z[4] - '0');

	if (!year || !month || month > 12 || !day || hour > 23 ||
	    minute > 59 || second > 60 || zone_hour > 23 || zone_minute > 59)
		return 0;
	unsigned leap = !(year % 4) && (year % 100 || !(year % 400));
	return day <= days[month - 1] + (month == 2 && leap);
}

static int format_line(const logger_t *l, const logger_message_t *m,
		       char *out, size_t cap, size_t *length, int strict)
{
	if (!cap)
		return strict ? -EOVERFLOW : 0;
	if (!out || !l || !m)
		return -EINVAL;
	out[0] = 0;
	if (cap == 1)
		return strict ? -EOVERFLOW : 0;
	if (strict && (m->ts.tv_nsec < 0 || m->ts.tv_nsec >= 1000000000L))
		return -EINVAL;

	time_t sec = m->ts.tv_sec;
	if (!timestamp.valid || timestamp.sec != sec) {
		struct tm tm;

		/* Invalidate before touching buffers. A failed refresh must not make
		 * partial new bytes appear as a valid old-second cache hit. */
		timestamp.valid = 0;
		if (!localtime_r(&sec, &tm) ||
		    !strftime(timestamp.date, sizeof(timestamp.date),
			      "%Y-%m-%dT%H:%M:%S", &tm) ||
		    !strftime(timestamp.zone, sizeof(timestamp.zone), "%z", &tm)) {
			/* Ordinary diagnostic output keeps its historical visible fallback.
			 * Strict output rejects below, before any backend operation. */
			memcpy(timestamp.date, "time-unavailable", sizeof("time-unavailable"));
			timestamp.zone[0] = 0;
		} else {
			timestamp.sec = sec;
			timestamp.valid = 1;
		}
	}
	if (strict && !canonical_time(&timestamp))
		return -EOVERFLOW;

	size_t used = 0;
	int rc, truncated = 0;
#define ADD(...) do { \
	rc = append(out, cap - 1, &used, __VA_ARGS__); \
	if (strict && rc) \
		return rc; \
	if (rc == -EOVERFLOW) \
		truncated = 1; \
} while (0)
	ADD("%s.%06ld%s %-5s", timestamp.date, m->ts.tv_nsec / 1000L,
	    timestamp.zone, logger_level_name(m->level));
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
	if (truncated)
		logger_text_mark_truncated(out, cap - 1);
	out[used++] = '\n';
	out[used] = 0;
	*length = used;
	return 0;
}

size_t logger_format_line(const logger_t *l, const logger_message_t *m,
			  char *out, size_t cap)
{
	size_t length = 0;
	int rc = format_line(l, m, out, cap, &length, 0);

	if (rc)
		errno = -rc;
	return length;
}

int logger_format_line_checked(const logger_t *l, const logger_message_t *m,
			       char *out, size_t cap, size_t *length)
{
	if (!length)
		return -EINVAL;
	*length = 0;
	int rc = format_line(l, m, out, cap, length, 1);

	if (rc && out && cap)
		out[0] = 0;
	return rc;
}
