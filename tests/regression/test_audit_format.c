#define _GNU_SOURCE
#include "audit.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { \
	if (!(test)) { \
		fprintf(stderr, "%s:%d: %s (errno=%d)\n", \
			__FILE__, __LINE__, #test, errno); \
		exit(1); \
	} \
} while (0)

enum fault_kind { NONE, LOCALTIME, DATE, ZONE, BAD_DATE, BAD_ZONE };
static enum fault_kind fault;
static time_t now = 1700000000;
static unsigned hits;

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
	/* Do not fail file-open wall_day(time(NULL)); target record formatting. */
	if (fault == LOCALTIME && *sec == now) {
		++hits;
		errno = EOVERFLOW;
		return NULL;
	}
	return __real_localtime_r(sec, out);
}

size_t __wrap_strftime(char *out, size_t cap, const char *fmt,
		      const struct tm *tm)
{
	int date = !strcmp(fmt, "%Y-%m-%dT%H:%M:%S");
	int zone = !strcmp(fmt, "%z");

	if ((fault == DATE && date) || (fault == ZONE && zone)) {
		++hits;
		/* strftime's output is unspecified on failure; poison it deliberately. */
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
	struct snapshot *after = malloc(sizeof(*after));

	CHECK(after);
	snapshot(path, after);
	CHECK(after->size == before->size);
	CHECK(!memcmp(after->bytes, before->bytes, before->size));
	free(after);
}

static logger_t *ordinary_logger(const char *path)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.async_mode = 0;
	c.outputs = LOGGER_OUT_FILE;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.file_path = path;
	return logger_create(&c);
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

static void check_truncation(const char *path)
{
	logger_t *l = ordinary_logger(path);
	char *module = malloc(32768);
	struct stat st;

	CHECK(l && module);
	memset(module, 'M', 32767);
	module[32767] = 0;
	CHECK(logger_log_sync_status(l, LOGGER_INFO, module, NULL, 0,
				     NULL, "body") == -EOVERFLOW);
	CHECK(!stat(path, &st) && st.st_size == 0);
	logger_log(l, LOGGER_INFO, module, NULL, 0, NULL, "body");
	CHECK(!stat(path, &st) && st.st_size > 0);
	CHECK(!logger_destroy_status(l));
	free(module);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *scenario = argv[1];
	char dir[] = "/tmp/c-logger-format-XXXXXX";
	char log_path[512];
	char state_path[512];
	char normal_path[512];

	CHECK(mkdtemp(dir));
	CHECK(snprintf(log_path, sizeof(log_path), "%s/probe.audit.log", dir) > 0);
	CHECK(snprintf(state_path, sizeof(state_path), "%s/probe.audit.state", dir) > 0);
	CHECK(snprintf(normal_path, sizeof(normal_path), "%s/normal.log", dir) > 0);
	if (!strcmp(scenario, "truncation")) {
		check_truncation(normal_path);
		clean_dir(dir);
		puts("audit format: truncation PASS");
		return 0;
	}

	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = dir;
	c.name = "probe";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(!audit_init(&c));

	struct snapshot *log_before = malloc(sizeof(*log_before));
	struct snapshot *state_before = malloc(sizeof(*state_before));
	CHECK(log_before && state_before);
	snapshot(log_path, log_before);
	snapshot(state_path, state_before);
	CHECK(!audit_verify_file(log_path));

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
		/* A successful ordinary strftime can cache bytes outside Audit grammar. */
		logger_t *l = ordinary_logger(normal_path);
		CHECK(l);
		logger_log(l, LOGGER_INFO, "ordinary", NULL, 0, NULL, "warm-cache");
		CHECK(!logger_destroy_status(l));
		CHECK(hits == 1);
		fault = NONE;
	}

	audit_event_t e = {
		.phase = AUDIT_PHASE_RESULT, .event = "CHECK",
		.actor = "tester", .operation = "write", .result = AUDIT_SUCCESS
	};
	errno = 0;
	int rc = audit_write(&e);
	int error = errno;
	unsigned observed_hits = hits;
	fault = NONE;
	audit_status_t status;
	CHECK(!audit_get_status(&status));
	CHECK(!audit_verify_file(log_path));
	if (normal || cache_hit) {
		CHECK(rc == 0);
		CHECK(status.state == AUDIT_STATE_RUNNING);
		CHECK(status.committed_seq == 2 && status.checkpoint_seq == 2);
		CHECK(!observed_hits);
		CHECK(!audit_shutdown_status());
	} else {
		CHECK(rc == -1 && error == EOVERFLOW);
		CHECK(observed_hits == 1);
		CHECK(status.state == AUDIT_STATE_IO_FAILED);
		CHECK(status.error_code == EOVERFLOW);
		CHECK(status.committed_seq == 1 && status.checkpoint_seq == 1);
		CHECK(!status.checkpoint_dirty);
		unchanged(log_path, log_before);
		unchanged(state_path, state_before);
		errno = 0;
		CHECK(audit_shutdown_status() == -1 && errno == EOVERFLOW);
		unchanged(log_path, log_before);
		unchanged(state_path, state_before);
	}

	/* Healthy time on a new second must allow verify/recovery/new session. */
	++now;
	CHECK(!audit_init(&c));
	CHECK(!audit_write(&e));
	CHECK(!audit_shutdown_status());
	CHECK(!audit_verify_file(log_path));

	/* Ordinary logging retains visible fallback, including after strict errors. */
	++now;
	logger_t *l = ordinary_logger(normal_path);
	CHECK(l);
	fault = LOCALTIME;
	logger_log(l, LOGGER_INFO, "ordinary", NULL, 0, NULL, "fallback");
	fault = NONE;
	CHECK(!logger_destroy_status(l));
	snapshot(normal_path, log_before);
	CHECK(memmem(log_before->bytes, log_before->size,
		     "time-unavailable", strlen("time-unavailable")));
	free(log_before);
	free(state_before);
	clean_dir(dir);
	printf("audit format: %s PASS (fault_hits=%u)\n", scenario, observed_hits);
	return 0;
}
