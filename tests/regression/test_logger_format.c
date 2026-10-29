#define _GNU_SOURCE
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test)                                                         \
	do {                                                                \
		if (!(test)) {                                              \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, \
				__LINE__, #test, errno);                    \
			exit(1);                                            \
		}                                                           \
	} while (0)

enum fault_kind { NONE, LOCALTIME, DATE, ZONE, BAD_DATE, BAD_ZONE };
static enum fault_kind fault;
static time_t now = 1700000000;
static unsigned int hits;
static unsigned int localtime_calls, date_calls, zone_calls;

int __real_clock_gettime(clockid_t id, struct timespec *ts);
struct tm *__real_localtime_r(const time_t *sec, struct tm *out);
size_t __real_strftime(char *out, size_t cap, const char *fmt,
		       const struct tm *tm);

int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
	int rc = __real_clock_gettime(id, ts);

	if (!rc && id == CLOCK_REALTIME) {
		ts->tv_sec = now;
		ts->tv_nsec = 123456000;
	}
	return rc;
}

struct tm *__wrap_localtime_r(const time_t *sec, struct tm *out)
{
	/* 不要使文件打开时的 wall_day(time(NULL)) 失败；目标是记录格式化。 */
	if (*sec == now) {
		++localtime_calls;
		if (fault == LOCALTIME) {
			++hits;
			errno = EOVERFLOW;
			return NULL;
		}
	}
	return __real_localtime_r(sec, out);
}

size_t __wrap_strftime(char *out, size_t cap, const char *fmt,
		       const struct tm *tm)
{
	int date = !strcmp(fmt, "%Y-%m-%dT%H:%M:%S");
	int zone = !strcmp(fmt, "%z");

	date_calls += (unsigned int)date;
	zone_calls += (unsigned int)zone;
	if ((fault == DATE && date) || (fault == ZONE && zone)) {
		++hits;
		/* strftime 失败时输出未指定；有意污染它。 */
		memset(out, 'X', cap);
		return 0;
	}
	if ((fault == BAD_DATE && date) || (fault == BAD_ZONE && zone)) {
		const char *bad = date ? "0000-01-01T00:00:00" : "+2460";
		size_t n = strlen(bad);

		CHECK(cap > n);
		memcpy(out, bad, n + 1);
		++hits;
		return n;
	}
	return __real_strftime(out, cap, fmt, tm);
}

struct snapshot {
	unsigned char bytes[16384];
	size_t size;
};

static void snapshot(const char *path, struct snapshot *out)
{
	FILE *f = fopen(path, "rb");

	CHECK(f);
	out->size = fread(out->bytes, 1, sizeof(out->bytes), f);
	CHECK(out->size < sizeof(out->bytes));
	CHECK(!ferror(f) && feof(f));
	CHECK(!fclose(f));
}

static void unchanged(const char *path, const struct snapshot *before)
{
	struct snapshot after;

	snapshot(path, &after);
	CHECK(after.size == before->size);
	CHECK(!memcmp(after.bytes, before->bytes, before->size));
}

static void check_record(const char *path, const struct snapshot *before,
			 const char *stamp, const char *body)
{
	struct snapshot after;
	char expected[512], timestamp[96], date[32], zone[16];
	struct tm tm;

	if (!stamp) {
		CHECK(__real_localtime_r(&now, &tm));
		CHECK(__real_strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S",
				      &tm));
		CHECK(__real_strftime(zone, sizeof(zone), "%z", &tm));
		CHECK(snprintf(timestamp, sizeof(timestamp), "%s.123456%s",
			       date, zone) > 0);
		stamp = timestamp;
	}
	int n = snprintf(expected, sizeof(expected),
			 "%s INFO  [formatter] %s\n", stamp, body);
	CHECK(n > 0 && (size_t)n < sizeof(expected));
	snapshot(path, &after);
	CHECK(after.size == before->size + (size_t)n);
	CHECK(!memcmp(after.bytes, before->bytes, before->size));
	CHECK(!memcmp(after.bytes + before->size, expected, (size_t)n));
}

static int strict_record(logger_t *l, const char *body)
{
	return logger_log_sync_status(l, LOGGER_INFO, "formatter", NULL, 0,
				      NULL, "%s", body);
}

static logger_t *ordinary_logger(const char *path)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.async_mode = 0;
	c.outputs = LOGGER_OUT_FILE;
	c.detail = LOGGER_DETAIL_NORMAL;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.file_path = path;
	logger_t *created = NULL;
	(void)logger_create(&c, &created);
	return created;
}

static void clean_dir(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *entry;

	CHECK(d);
	for (;;) {
		errno = 0;
		entry = readdir(d);
		if (!entry) {
			CHECK(!errno);
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		CHECK(!unlinkat(dirfd(d), entry->d_name, 0));
	}
	CHECK(!closedir(d));
	CHECK(!rmdir(path));
}

static void check_truncated(const char *path, const struct snapshot *before)
{
	static const char marker[] = " [truncated]\n";
	struct snapshot after;

	snapshot(path, &after);
	CHECK(after.size > before->size + sizeof(marker) - 1);
	CHECK(!memcmp(after.bytes, before->bytes, before->size));
	CHECK(!memcmp(after.bytes + after.size - (sizeof(marker) - 1), marker,
		      sizeof(marker) - 1));
	CHECK(!memchr(after.bytes + before->size, '\0',
		      after.size - before->size));
	CHECK(!memchr(after.bytes + before->size, '\n',
		      after.size - before->size - 1));
}

static void check_truncation(const char *path)
{
	logger_t *l = ordinary_logger(path);
	char *oversized = malloc(32768);
	struct snapshot before;

	CHECK(l && oversized);
	memset(oversized, 'M', 32767);
	oversized[32767] = 0;
	snapshot(path, &before);
	CHECK(!before.size);
	CHECK(logger_log_sync_status(l, LOGGER_INFO, oversized, NULL, 0, NULL,
				     "body") == -EOVERFLOW);
	unchanged(path, &before);
	logger_log(l, LOGGER_INFO, oversized, NULL, 0, NULL, "body");
	check_truncated(path, &before);

	/* 消息溢出以及行或元数据溢出都会在输出前失败。 */
	snapshot(path, &before);
	CHECK(strict_record(l, oversized) == -EOVERFLOW);
	unchanged(path, &before);
	logger_log(l, LOGGER_INFO, "formatter", NULL, 0, NULL, "%s", oversized);
	check_truncated(path, &before);
	snapshot(path, &before);
	CHECK(!strict_record(l, "after-truncation"));
	check_record(path, &before, NULL, "after-truncation");
	logger_state_t state;
	CHECK(logger_get_state(l, &state) == 0 &&
	      state == LOGGER_STATE_RUNNING);
	CHECK(!logger_destroy(&l));
	free(oversized);
}

int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	const char *scenario = argv[1];
	char dir[] = "/tmp/c-logger-format-XXXXXX";
	char log_path[512];
	struct snapshot before;

	CHECK(!setenv("TZ", "UTC0", 1));
	tzset();
	CHECK(mkdtemp(dir));
	CHECK(snprintf(log_path, sizeof(log_path), "%s/normal.log", dir) > 0);
	if (!strcmp(scenario, "truncation")) {
		check_truncation(log_path);
		clean_dir(dir);
		puts("logger format: truncation PASS");
		return 0;
	}

	logger_t *l = ordinary_logger(log_path);
	CHECK(l);
	snapshot(log_path, &before);
	CHECK(!before.size);
	CHECK(!strict_record(l, "warm-cache"));
	check_record(log_path, &before, NULL, "warm-cache");
	CHECK(localtime_calls == 1 && date_calls == 1 && zone_calls == 1);
	CHECK(!hits);

	int cache_hit = !strcmp(scenario, "cache-hit");
	int cached_bad = !strcmp(scenario, "cached-bad");
	int normal = !strcmp(scenario, "normal");
	if (!cache_hit)
		++now;
	if (normal)
		fault = NONE;
	else if (!strcmp(scenario, "localtime") || cache_hit)
		fault = LOCALTIME;
	else if (!strcmp(scenario, "date"))
		fault = DATE;
	else if (!strcmp(scenario, "zone"))
		fault = ZONE;
	else if (!strcmp(scenario, "bad-date") || cached_bad)
		fault = BAD_DATE;
	else if (!strcmp(scenario, "bad-zone"))
		fault = BAD_ZONE;
	else
		CHECK(0);

	if (cached_bad) {
		/*
		 * 普通成功的 strftime 可能缓存不规范字节。
		 * 即使没有新的转换，严格日志也必须验证这些字节。
		 */
		snapshot(log_path, &before);
		logger_log(l, LOGGER_INFO, "formatter", NULL, 0, NULL,
			   "warm-bad-cache");
		check_record(log_path, &before,
			     "0000-01-01T00:00:00.123456+0000",
			     "warm-bad-cache");
		CHECK(hits == 1);
		CHECK(localtime_calls == 2 && date_calls == 2 &&
		      zone_calls == 2);
		fault = NONE;
	}

	snapshot(log_path, &before);
	int rc = strict_record(l, "strict-probe");
	unsigned int observed_hits = hits;
	if (normal || cache_hit) {
		CHECK(!rc && !observed_hits);
		check_record(log_path, &before, NULL, "strict-probe");
		unsigned int expected_calls = cache_hit ? 1 : 2;
		CHECK(localtime_calls == expected_calls);
		CHECK(date_calls == expected_calls &&
		      zone_calls == expected_calls);
	} else {
		CHECK(rc == -EOVERFLOW && observed_hits == 1);
		unchanged(log_path, &before);
		CHECK(localtime_calls == 2);
		CHECK(date_calls == (!strcmp(scenario, "localtime") ? 1u : 2u));
		CHECK(zone_calls == (!strcmp(scenario, "localtime") ||
						     !strcmp(scenario, "date") ?
					     1u :
					     2u));
		if (!strcmp(scenario, "bad-date") ||
		    !strcmp(scenario, "bad-zone") || cached_bad) {
			fault = NONE;
			/*
			 * 移除“格式畸形但成功”的故障不会刷新有效的同秒缓存。
			 * 它必须继续被拒绝且不输出。
			 */
			CHECK(strict_record(l, "cached-rejection") ==
			      -EOVERFLOW);
			unchanged(log_path, &before);
			CHECK(localtime_calls == 2 && date_calls == 2 &&
			      zone_calls == 2);
			CHECK(hits == observed_hits);
		} else {
			/*
			 * 也通过普通输出检查每种转换失败：
			 * 即使 strftime 缓冲区被污染，也必须产生可见回退文本。
			 */
			logger_log(l, LOGGER_INFO, "formatter", NULL, 0, NULL,
				   "fault-fallback");
			check_record(log_path, &before,
				     "time-unavailable.123456",
				     "fault-fallback");
			CHECK(hits == observed_hits + 1);
			fault = NONE;

			/*
			 * 刷新失败也必须使前一秒失效。
			 * 恢复前再次访问它，以捕获部分或陈旧缓存命中。
			 */
			--now;
			snapshot(log_path, &before);
			unsigned int old_localtime = localtime_calls;
			CHECK(!strict_record(l, "old-second-recovery"));
			check_record(log_path, &before, NULL,
				     "old-second-recovery");
			CHECK(localtime_calls == old_localtime + 1);
			++now;
			snapshot(log_path, &before);
			old_localtime = localtime_calls;
			CHECK(!strict_record(l, "same-second-recovery"));
			check_record(log_path, &before, NULL,
				     "same-second-recovery");
			CHECK(localtime_calls == old_localtime + 1);
		}
	}
	logger_state_t state;
	CHECK(logger_get_state(l, &state) == 0 &&
	      state == LOGGER_STATE_RUNNING);

	/* 健康的下一秒能恢复，即使缓存来自格式畸形但成功的转换。 */
	fault = NONE;
	++now;
	snapshot(log_path, &before);
	unsigned int old_localtime = localtime_calls;
	CHECK(!strict_record(l, "next-second-recovery"));
	check_record(log_path, &before, NULL, "next-second-recovery");
	CHECK(localtime_calls == old_localtime + 1);

	/*
	 * 严格时间失败后，普通日志保持可见回退文本，
	 * 回退本身也绝不能成为有效的同秒缓存命中。
	 */
	++now;
	fault = LOCALTIME;
	snapshot(log_path, &before);
	unsigned int old_hits = hits;
	CHECK(strict_record(l, "no-partial-fallback") == -EOVERFLOW);
	unchanged(log_path, &before);
	logger_log(l, LOGGER_INFO, "formatter", NULL, 0, NULL, "fallback");
	check_record(log_path, &before, "time-unavailable.123456", "fallback");
	CHECK(hits == old_hits + 2);
	fault = NONE;
	snapshot(log_path, &before);
	old_localtime = localtime_calls;
	CHECK(!strict_record(l, "after-fallback"));
	check_record(log_path, &before, NULL, "after-fallback");
	CHECK(localtime_calls == old_localtime + 1);
	CHECK(!logger_destroy(&l));
	clean_dir(dir);
	printf("logger format: %s PASS (fault_hits=%u)\n", scenario,
	       observed_hits);
	return 0;
}
