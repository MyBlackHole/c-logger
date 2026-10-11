#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#ifndef TEST_EXPECT_HOOKS
#define TEST_EXPECT_HOOKS 0
#endif
extern char **environ;
static int intercept_connect;
static char connected_path[108];
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *address, socklen_t n)
{
	if (!intercept_connect)
		return __real_connect(fd, address, n);
	CHECK(address->sa_family == AF_UNIX);
	const struct sockaddr_un *un = (const struct sockaddr_un *)address;
	snprintf(connected_path, sizeof(connected_path), "%s", un->sun_path);
	errno = ENOENT;
	return -1;
}
static logger_config_t log_config(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = 0;
	return c;
}
static void clear_test_environment(void)
{
	unsetenv("LOGGER_FAULT_POINT");
	unsetenv("LOGGER_FAULT_AFTER");
	unsetenv("LOGGER_FAULT_ERRNO");
	unsetenv("LOGGER_FAULT_SHORT_WRITE");
	unsetenv("LOGGER_CRASH_POINT");
	unsetenv("LOGGER_SYSLOG_PATH");
}
static void scenario(const char *mode)
{
	clear_test_environment();
	unlink("out.log");
	logger_config_t c = log_config();
	CHECK(setenv("LOGGER_FAULT_ERRNO", "5", 1) == 0);
	if (!strcmp(mode, "syslog-path")) {
		CHECK(!setenv("LOGGER_SYSLOG_PATH", "./redirect.socket", 1));
		intercept_connect = 1;
		c.outputs = LOGGER_OUT_SYSLOG;
		CHECK(logger_create(&c) == NULL && errno == ENOENT);
		CHECK(!strcmp(connected_path, TEST_EXPECT_HOOKS ?
						      "./redirect.socket" :
						      "/dev/log"));
	} else if (!strncmp(mode, "file_", 5) ) {
		logger_t *l;
		if (!strcmp(mode, "file_rename")) {
			c.rotation.mode = LOGGER_ROTATE_SIZE;
			c.rotation.max_file_size = 1;
		}
		CHECK((l = logger_create(&c)) != NULL);
		if (!strcmp(mode, "file_rename"))
			CHECK(logger_log_sync_status(l, LOGGER_INFO, "m", NULL,
						     0, NULL, "baseline") == 0);
		CHECK(!setenv("LOGGER_FAULT_POINT", mode, 1));
		int rc = logger_log_sync_status(l, LOGGER_INFO, "m", NULL, 0,
						NULL, "probe");
		CHECK(rc == (TEST_EXPECT_HOOKS ? -EIO : 0));
		logger_destroy(l);
	} else if (!strcmp(mode, "short-write")) {
		CHECK(!setenv("LOGGER_FAULT_SHORT_WRITE", "3", 1));
		logger_t *l = logger_create(&c);
		CHECK(l);
		CHECK(!logger_log_sync_status(l, LOGGER_INFO, "m", NULL, 0,
					      NULL, "short write complete"));
		logger_destroy(l);
		CHECK(file_contains("out.log", "short write complete"));
	} else {
		/* Both variants execute the same path; support MUST terminate 197 at
         * the requested point, production MUST execute through successful output. */
		CHECK(!setenv("LOGGER_CRASH_POINT", mode, 1));
		logger_t *l = logger_create(&c);
		CHECK(l);
		CHECK(!logger_log_sync_status(l, LOGGER_INFO, "isolation", NULL,
					      0, NULL, "crash-point-probe"));
		CHECK(!logger_destroy_status(l));
		CHECK(file_contains("out.log", "crash-point-probe"));
	}
}
int main(int argc, char **argv)
{
	CHECK(argc == 2 || argc == 3);
	if (argc == 3) {
		scenario(argv[2]);
		return 0;
	}
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1);
	exe[n] = 0;
	char *const args[] = { exe, "--child", argv[1], NULL };
	pid_t p = fork();
	CHECK(p >= 0);
	if (!p) {
		execve(exe, args, environ);
		_exit(127);
	}
	int status;
	while (waitpid(p, &status, 0) < 0)
		CHECK(errno == EINTR);
	int crash = !strcmp(argv[1], "after_file_fsync");
	int expected = TEST_EXPECT_HOOKS && crash ? 197 : 0;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expected)
		fprintf(stderr, "child status=%d expected exit=%d\n", status,
			expected);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
	puts(TEST_EXPECT_HOOKS ?
		     "test-only hook behavior verified" :
		     "production ignores fault/crash/path environment");
	return 0;
}
