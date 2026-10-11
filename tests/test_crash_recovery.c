#define _GNU_SOURCE
#include "crash_support.h"
#include "regression/support.h"
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

static int notify_fd = -1;

_Noreturn void crash_fixture_cut(const char *point)
{
	size_t length = strlen(point);
	CHECK(notify_fd >= 0 && length < PIPE_BUF);
	CHECK(write(notify_fd, point, length) == (ssize_t)length);
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--verify")) {
		CHECK(!strcmp(argv[3], "0") || !strcmp(argv[3], "1") || !strcmp(argv[3], "2"));
		crash_fixture_verify(argv[2], (unsigned)(argv[3][0] - '0'));
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--writer")) {
		char *end;
		long fd = strtol(argv[3], &end, 10);
		CHECK(!*end && fd >= 0 && fd <= INT_MAX);
		notify_fd = (int)fd;
		crash_fixture_write(argv[2]);
		return 99;
	}
	CHECK(argc == 2 && crash_fixture_point_valid(argv[1]));
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1);
	exe[n] = 0;
	char directory[] = "/tmp/logger-crash-XXXXXX";
	enter_temp(directory);
	int notification[2];
	CHECK(pipe(notification) == 0);
	char fd[32];
	CHECK(snprintf(fd, sizeof(fd), "%d", notification[1]) > 0);
	char *args[] = { exe, "--writer", argv[1], fd, NULL };
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(notification[0]);
		execv(exe, args);
		_exit(127);
	}
	CHECK(close(notification[1]) == 0);
	struct pollfd ready = { .fd = notification[0], .events = POLLIN };
	int rc;
	do {
		rc = poll(&ready, 1, 10000);
	} while (rc < 0 && errno == EINTR);
	char observed[128] = { 0 };
	ssize_t size = rc > 0 ? read(notification[0], observed, sizeof(observed) - 1) : -1;
	int reached = size == (ssize_t)strlen(argv[1]) && !strcmp(observed, argv[1]);
	/* A timeout/missing/wrong marker fails even though cleanup kills the child. */
	CHECK(kill(child, SIGKILL) == 0);
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(close(notification[0]) == 0);
	CHECK(reached && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	printf("PROCESS_SIGKILL_CONFIRMED point=%s\n", argv[1]);
	crash_fixture_recover(argv[1]);
	crash_fixture_cleanup(directory);
	return 0;
}
