#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#ifndef TEST_EXPECT_HOOKS
#define TEST_EXPECT_HOOKS 0
#endif
extern char **environ;
static int intercept_connect;
static char connected_path[108];

/* Export this executable symbol for both production static and shared links.
 * --wrap=connect alone cannot observe a call made inside the production DSO.
 * Linux is the supported platform; forward unrelated calls without libdl. */
int isolation_connect(int, const struct sockaddr *, socklen_t) __asm__("connect");
int isolation_connect(int fd, const struct sockaddr *address, socklen_t n)
{
	if (!intercept_connect)
		return (int)syscall(SYS_connect, fd, address, n);
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

static int file_fault(const char *mode)
{
	return !strcmp(mode, "file_write") || !strcmp(mode, "file_fsync") ||
	       !strcmp(mode, "file_rename");
}

static int file_crash(const char *mode)
{
	/* The other four process-crash cases use syscall/acknowledgment probes,
	 * not environment-enabled library hooks. They have their own matrix. */
	return !strcmp(mode, "file_after_archive_rename") ||
	       !strcmp(mode, "file_after_archive_dirsync") ||
	       !strcmp(mode, "file_after_active_open") ||
	       !strcmp(mode, "file_after_active_dirsync");
}

static void clear_test_environment(void)
{
	CHECK(!unsetenv("LOGGER_FAULT_POINT"));
	CHECK(!unsetenv("LOGGER_FAULT_AFTER"));
	CHECK(!unsetenv("LOGGER_FAULT_ERRNO"));
	CHECK(!unsetenv("LOGGER_FAULT_SHORT_WRITE"));
	CHECK(!unsetenv("LOGGER_CRASH_POINT"));
	CHECK(!unsetenv("LOGGER_SYSLOG_PATH"));
}

static int emit(logger_t *logger, const char *message)
{
	return logger_log_sync_status(logger, LOGGER_INFO, "isolation", NULL, 0,
				      NULL, "%s", message);
}

static void scenario(const char *mode)
{
	clear_test_environment();
	logger_config_t c = log_config();
	CHECK(!setenv("LOGGER_FAULT_ERRNO", "5", 1));
	if (!strcmp(mode, "syslog-path")) {
		CHECK(!setenv("LOGGER_SYSLOG_PATH", "./redirect.socket", 1));
		intercept_connect = 1;
		c.outputs = LOGGER_OUT_SYSLOG;
		CHECK(logger_create(&c) == NULL && errno == ENOENT);
		CHECK(!strcmp(connected_path, TEST_EXPECT_HOOKS ?
						      "./redirect.socket" :
						      "/dev/log"));
	} else if (file_fault(mode)) {
		if (!strcmp(mode, "file_rename")) {
			c.rotation.mode = LOGGER_ROTATE_SIZE;
			c.rotation.max_file_size = 1;
		}
		logger_t *logger = logger_create(&c);
		CHECK(logger);
		if (!strcmp(mode, "file_rename"))
			CHECK(!emit(logger, "baseline"));
		CHECK(!setenv("LOGGER_FAULT_POINT", mode, 1));
		int rc = emit(logger, "probe");
		CHECK(rc == (TEST_EXPECT_HOOKS ? -EIO : 0));
		clear_test_environment();
		CHECK(!emit(logger, "after-fault"));
		CHECK(logger_destroy_status(logger) ==
		      (TEST_EXPECT_HOOKS ? -1 : 0));
		CHECK(file_contains("out.log", "after-fault"));
	} else if (!strcmp(mode, "short-write")) {
		CHECK(!setenv("LOGGER_FAULT_SHORT_WRITE", "3", 1));
		logger_t *logger = logger_create(&c);
		CHECK(logger);
		CHECK(!emit(logger, "short write complete"));
		logger_file_metrics_t metrics;
		CHECK(!logger_get_file_metrics(logger, &metrics));
		CHECK(metrics.bytes_written > 3 &&
		      metrics.write_operations == 1);
		/* Sync logging uses write, and its public syscall counter proves
		 * support actually split the record while production ignored it. */
		CHECK(TEST_EXPECT_HOOKS ? metrics.write_syscalls > 1 :
					 metrics.write_syscalls == 1);
		CHECK(!logger_destroy_status(logger));
		CHECK(file_contains("out.log", "short write complete"));
	} else {
		CHECK(file_crash(mode));
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		c.rotation.max_file_size = 1;
		logger_t *logger = logger_create(&c);
		CHECK(logger);
		CHECK(!emit(logger, "baseline"));
		/* Both variants take a real file rotation. Support must terminate
		 * at the named hook; production must complete the switch and write. */
		CHECK(!setenv("LOGGER_CRASH_POINT", mode, 1));
		CHECK(!emit(logger, "after-rotation"));
		logger_file_metrics_t metrics;
		CHECK(!logger_get_file_metrics(logger, &metrics));
		CHECK(metrics.rotation_successes == 1);
		CHECK(!logger_destroy_status(logger));
		CHECK(file_contains("out.log", "after-rotation"));
	}
}

static void cleanup_fixture(const char *directory, int original)
{
	DIR *dir = opendir(".");
	CHECK(dir);
	struct dirent *entry;
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			CHECK(!errno);
			break;
		}
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			CHECK(!unlink(entry->d_name));
	}
	CHECK(!closedir(dir));
	CHECK(!fchdir(original) && !close(original));
	CHECK(!rmdir(directory));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2 || (argc == 3 && !strcmp(argv[1], "--child")));
	if (argc == 3) {
		scenario(argv[2]);
		return 0;
	}
	CHECK(file_fault(argv[1]) || file_crash(argv[1]) ||
	      !strcmp(argv[1], "short-write") || !strcmp(argv[1], "syslog-path"));
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1);
	exe[n] = 0;
	int original = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(original >= 0);
	char directory[] = "/tmp/logger-isolation-XXXXXX";
	enter_temp(directory);
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
	int expected = TEST_EXPECT_HOOKS && file_crash(argv[1]) ? 197 : 0;
	cleanup_fixture(directory, original);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expected)
		fprintf(stderr, "child status=%d expected exit=%d\n", status,
			expected);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
	puts(TEST_EXPECT_HOOKS ?
		     "test-only hook behavior verified" :
		     "production ignores fault/crash/path environment");
	return 0;
}
