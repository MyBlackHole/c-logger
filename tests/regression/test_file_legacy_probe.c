/* Black-box regression assertions using only pre-existing public APIs.
 * Compile and link this identical source against both the input and output
 * archives. A nonzero result on the input demonstrates the old defect. */
#define _GNU_SOURCE
#include "logger.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define REQUIRE(x)                                                       \
	do {                                                             \
		if (!(x)) {                                              \
			fprintf(stderr, "%s:%d %s errno=%d\n", __FILE__, \
				__LINE__, #x, errno);                    \
			return 1;                                        \
		}                                                        \
	} while (0)
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t c, struct timespec *t)
{
	if (c == CLOCK_REALTIME) {
		*t = (struct timespec){ 1700000000, 0 };
		return 0;
	}
	return __real_clock_gettime(c, t);
}
static void put(const char *p, const char *s)
{
	FILE *f = fopen(p, "w");
	if (!f)
		abort();
	if (fputs(s, f) < 0 || fclose(f))
		abort();
}
static int has(const char *p, const char *s)
{
	FILE *f = fopen(p, "r");
	if (!f)
		return 0;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	fclose(f);
	return strstr(b, s) != NULL;
}
static char fixture[] = "/tmp/logger-file-blackbox-XXXXXX";
static void remove_tree(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char child[4096];
		int n = snprintf(child, sizeof(child), "%s/%s", path,
				 e->d_name);
		if (n < 0 || (size_t)n >= sizeof(child))
			abort();
		struct stat st;
		if (lstat(child, &st))
			abort();
		if (S_ISDIR(st.st_mode)) {
			remove_tree(child);
			if (rmdir(child))
				abort();
		} else if (unlink(child))
			abort();
	}
	if (closedir(d))
		abort();
}
static void cleanup_fixture(void)
{
	if (chdir("/"))
		abort();
	remove_tree(fixture);
	if (rmdir(fixture))
		abort();
}
int main(int argc, char **argv)
{
	REQUIRE(argc == 2);
	REQUIRE(mkdtemp(fixture) != NULL);
	REQUIRE(atexit(cleanup_fixture) == 0 && chdir(fixture) == 0);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	const char *s = argv[1];
	if (!strcmp(s, "symlink")) {
		put("sentinel", "unchanged");
		REQUIRE(symlink("sentinel", "out.log") == 0);
		logger_t *l = logger_create(&c);
		REQUIRE(!l && errno == ELOOP);
		return 0;
	}
	if (!strcmp(s, "hardlink")) {
		put("sentinel", "unchanged");
		REQUIRE(link("sentinel", "out.log") == 0);
		logger_t *l = logger_create(&c);
		REQUIRE(!l && errno == EMLINK);
		return 0;
	}
	if (!strcmp(s, "global-repeat")) {
		REQUIRE(logger_init(&c) == 0);
		c.file_path = "unwanted.log";
		REQUIRE(logger_init(&c) == -1 && errno == EALREADY);
		logger_shutdown();
		REQUIRE(access("unwanted.log", F_OK) != 0);
		return 0;
	}
	if (!strcmp(s, "collision")) {
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		c.rotation.max_file_size = 1;
		c.rotation.retention_days = 0;
		put("out.20231114T221320.000000Z.log", "sentinel");
	}
	logger_t *l = logger_create(&c);
	REQUIRE(l);
	REQUIRE(logger_log_sync_status(l, LOGGER_INFO, "probe", __FILE__,
				       __LINE__, __func__, "first") == 0);
	if (!strcmp(s, "owner")) {
		logger_t *other = logger_create(&c);
		REQUIRE(!other && errno == EBUSY);
	} else if (!strcmp(s, "reopen")) {
		REQUIRE(rename("out.log", "old.log") == 0 &&
			mkdir("out.log", 0700) == 0);
		REQUIRE(logger_reopen_instance(l) == -1);
		LOGGER_INFO(l, "probe", "after-failed-reopen");
		REQUIRE(has("old.log", "after-failed-reopen"));
	} else if (!strcmp(s, "cwd")) {
		REQUIRE(mkdir("newdir", 0700) == 0 && chdir("newdir") == 0);
		REQUIRE(logger_reopen_instance(l) == 0);
		LOGGER_INFO(l, "probe", "same-target");
		REQUIRE(access("out.log", F_OK) != 0 &&
			has("../out.log", "same-target"));
	} else if (!strcmp(s, "collision")) {
		REQUIRE(logger_log_sync_status(l, LOGGER_INFO, "probe",
					       __FILE__, __LINE__, __func__,
					       "second") == 0);
		REQUIRE(has("out.20231114T221320.000000Z.log", "sentinel"));
	} else
		REQUIRE(0);
	logger_destroy(l);
	return 0;
}
