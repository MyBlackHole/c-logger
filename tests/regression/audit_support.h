#ifndef AUDIT_REGRESSION_SUPPORT_H
#define AUDIT_REGRESSION_SUPPORT_H
#include "audit.h"
#include "support.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>

static inline audit_config_t audit_test_config(void)
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "app";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.failure_policy = AUDIT_FAIL_DENY;
	return c;
}

static inline audit_event_t audit_test_event(const char *name)
{
	audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
			    .event = name,
			    .actor = "user",
			    .source = "test",
			    .resource = "item",
			    .operation = "update",
			    .result = AUDIT_SUCCESS };
	return e;
}

static inline void audit_test_cleanup(const char *path)
{
	DIR *d = opendir(".");
	CHECK(d != NULL);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			CHECK(unlink(e->d_name) == 0);
	}
	CHECK(closedir(d) == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(path) == 0);
}

static inline unsigned count_event(const char *name)
{
	char marker[160];
	CHECK(snprintf(marker, sizeof(marker), " event=\"%s\"", name) > 0);
	FILE *f = fopen("app.audit.log", "r");
	CHECK(f != NULL);
	char line[8192];
	unsigned count = 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, marker))
			++count;
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return count;
}

/* This executable must dispatch --probe-lock before its ordinary test body.
 * Lock attempts run after exec, never by continuing an inherited Audit runtime. */
static inline int audit_probe_lock(const char *expect)
{
	int fd = open("app.audit.lock", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	struct flock lk = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	int rc = fcntl(fd, F_SETLK, &lk);
	int error = errno;
	CHECK(close(fd) == 0);
	if (!strcmp(expect, "busy"))
		CHECK(rc == -1 && (error == EAGAIN || error == EACCES));
	else
		CHECK(rc == 0);
	return 0;
}

static inline void check_process_lock(const char *expect)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1);
	exe[n] = '\0';
	char *argv[] = { exe, "--probe-lock", (char *)expect, NULL };
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		execv(exe, argv);
		_exit(127);
	}
	int st;
	CHECK(waitpid(child, &st, 0) == child);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
#endif
