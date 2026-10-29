#include "logger_libc.h"
#include "logger_internal.h"
#include "logger_compiler.h"
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *logger_level_name(logger_level_t level)
{
	static const char *names[] = { "TRACE", "DEBUG", "INFO", "WARN",
				       "ERROR", "FATAL", "OFF" };
	return (unsigned int)level <= LOGGER_OFF ? names[level] : "UNKNOWN";
}

const char *logger_basename(const char *path)
{
	const char *slash = path ? strrchr(path, '/') : NULL;

	return slash ? slash + 1 : (path ? path : "?");
}

struct line_buffer {
	char *out;
	size_t cap;
	size_t used;
	bool strict;
	bool truncated;
};

/*
 * cap 为 LF 预留空间；used 统计已存入的字节，而非原本需要写入的字节。
 * 普通输出标记截断；带确认的输出拒绝截断。
 */
static int LOGGER_PRINTF(2, 3)
	append(struct line_buffer *line, const char *fmt, ...)
{
	if (line->used >= line->cap - 1)
		goto overflow;
	va_list ap;

	va_start(ap, fmt);
	int n = logger_libc_vsnprintf(line->out + line->used,
				      line->cap - line->used, fmt, ap);
	va_end(ap);
	if (n < 0) {
		int rc = -(errno ? errno : EILSEQ);

		if (rc != -EOVERFLOW)
			return rc;
		goto overflow;
	}
	size_t room = line->cap - line->used;

	if ((size_t)n >= room) {
		line->used += room - 1;
		goto overflow;
	}
	line->used += (size_t)n;
	return 0;
overflow:
	if (line->strict)
		return -EOVERFLOW;
	line->truncated = true;
	return 0;
}

struct timestamp_cache {
	time_t sec;
	bool valid;
	char date[32];
	char zone[16];
};

static _Thread_local struct timestamp_cache timestamp;

/*
 * 也要验证缓存字节：普通调用可以填充同一缓存。
 * 仅 strftime 成功并不意味着符合严格的四位年份时间戳语法。
 */
static bool canonical_time(const struct timestamp_cache *cache)
{
	const char *d = cache->date;
	const char *z = cache->zone;
	static const unsigned int days[] = { 31, 28, 31, 30, 31, 30,
					     31, 31, 30, 31, 30, 31 };

	if (!cache->valid || strlen(d) != 19 || strlen(z) != 5)
		return false;
	for (unsigned int i = 0; i < 19; ++i) {
		char delimiter = i == 4 || i == 7   ? '-' :
				 i == 10	    ? 'T' :
				 i == 13 || i == 16 ? ':' :
						      0;
		if (delimiter ? d[i] != delimiter : d[i] < '0' || d[i] > '9')
			return false;
	}
	if (z[0] != '+' && z[0] != '-')
		return false;
	for (unsigned int i = 1; i < 5; ++i) {
		if (z[i] < '0' || z[i] > '9')
			return false;
	}

	unsigned int year = (unsigned int)(d[0] - '0') * 1000u +
			    (unsigned int)(d[1] - '0') * 100u +
			    (unsigned int)(d[2] - '0') * 10u +
			    (unsigned int)(d[3] - '0');
	unsigned int month =
		(unsigned int)(d[5] - '0') * 10u + (unsigned int)(d[6] - '0');
	unsigned int day =
		(unsigned int)(d[8] - '0') * 10u + (unsigned int)(d[9] - '0');
	unsigned int hour =
		(unsigned int)(d[11] - '0') * 10u + (unsigned int)(d[12] - '0');
	unsigned int minute =
		(unsigned int)(d[14] - '0') * 10u + (unsigned int)(d[15] - '0');
	unsigned int second =
		(unsigned int)(d[17] - '0') * 10u + (unsigned int)(d[18] - '0');
	unsigned int zone_hour =
		(unsigned int)(z[1] - '0') * 10u + (unsigned int)(z[2] - '0');
	unsigned int zone_minute =
		(unsigned int)(z[3] - '0') * 10u + (unsigned int)(z[4] - '0');

	if (!year || !month || month > 12 || !day || hour > 23 || minute > 59 ||
	    second > 60 || zone_hour > 23 || zone_minute > 59)
		return false;
	bool leap = !(year % 4) && (year % 100 || !(year % 400));

	return day <= days[month - 1] + (month == 2 && leap);
}

static int format_line(const logger_t *l, const struct logger_message *m,
		       char *out, size_t cap, size_t *length, bool strict)
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

		/*
		 * 操作缓冲区前先使缓存失效。刷新失败不能让部分新字节
		 * 看起来像有效的旧秒缓存命中。
		 */
		timestamp.valid = false;
		if (!logger_libc_localtime_r(&sec, &tm) ||
		    !logger_libc_strftime(timestamp.date,
					  sizeof(timestamp.date),
					  "%Y-%m-%dT%H:%M:%S", &tm) ||
		    !logger_libc_strftime(timestamp.zone,
					  sizeof(timestamp.zone), "%z", &tm)) {
			/*
			 * 普通诊断输出保留原有的可见回退文本。
			 * 严格输出会在下方、任何后端操作之前拒绝该情况。
			 */
			memcpy(timestamp.date, "time-unavailable",
			       sizeof("time-unavailable"));
			timestamp.zone[0] = 0;
		} else {
			timestamp.sec = sec;
			timestamp.valid = true;
		}
	}
	if (strict && !canonical_time(&timestamp))
		return -EOVERFLOW;

	struct line_buffer line = { .out = out,
				    .cap = cap - 1,
				    .strict = strict };
	int rc;

	rc = append(&line, "%s.%06ld%s %-5s", timestamp.date,
		    m->ts.tv_nsec / 1000L, timestamp.zone,
		    logger_level_name(m->level));
	if (rc)
		return rc;
	logger_detail_t detail = logger_internal_detail(l);

	if (detail >= LOGGER_DETAIL_VERBOSE) {
		if (logger_internal_include_pid(l)) {
			rc = append(&line, " pid=%ld", (long)m->pid);
			if (rc)
				return rc;
		}
		if (logger_internal_include_tid(l)) {
			rc = append(&line, " tid=%ld", (long)m->tid);
			if (rc)
				return rc;
		}
	}
	if (detail >= LOGGER_DETAIL_NORMAL) {
		rc = append(&line, " [%s]", m->module ? m->module : "app");
		if (rc)
			return rc;
	}
	if (detail >= LOGGER_DETAIL_DEBUG) {
		if (m->request_id[0]) {
			rc = append(&line, " request=%s", m->request_id);
			if (rc)
				return rc;
		}
		if (m->session_id[0]) {
			rc = append(&line, " session=%s", m->session_id);
			if (rc)
				return rc;
		}
		if (m->trace_id[0]) {
			rc = append(&line, " trace=%s", m->trace_id);
			if (rc)
				return rc;
		}
		if (logger_internal_include_source(l)) {
			rc = append(&line, " %s:%d %s()",
				    logger_basename(m->file), m->line,
				    m->func ? m->func : "?");
			if (rc)
				return rc;
		}
	}
	rc = append(&line, " %s", m->text);
	if (rc)
		return rc;
	if (line.truncated)
		logger_text_mark_truncated(out, cap - 1);
	out[line.used++] = '\n';
	out[line.used] = 0;
	*length = line.used;
	return 0;
}

size_t logger_format_line(const logger_t *l, const struct logger_message *m,
			  char *out, size_t cap)
{
	size_t length = 0;
	int rc = format_line(l, m, out, cap, &length, false);

	if (rc) {
		if (out && cap)
			out[0] = 0;
		errno = -rc;
	}
	return length;
}

int logger_format_line_checked(const logger_t *l,
			       const struct logger_message *m, char *out,
			       size_t cap, size_t *length)
{
	if (!length)
		return -EINVAL;
	*length = 0;
	int rc = format_line(l, m, out, cap, length, true);

	if (rc && out && cap)
		out[0] = 0;
	return rc;
}

void logger_format_reset_child(void)
{
	timestamp.valid = false;
}
