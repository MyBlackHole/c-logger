#define _GNU_SOURCE
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#define CHECK(x)                    \
	do {                        \
		if (!(x)) {         \
			perror(#x); \
			return 1;   \
		}                   \
	} while (0)
static int contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	int ok = !ferror(f) && strstr(b, needle) != NULL;
	fclose(f);
	return ok;
}
static int exec_child(void)
{
	/* Fresh image may use full APIs. No parent log fd may survive exec. */
	struct stat parent;
	CHECK(stat("fork-parent.log", &parent) == 0);
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	struct dirent *e;
	while ((e = readdir(d))) {
		char *end;
		long fd = strtol(e->d_name, &end, 10);
		if (*end || fd < 0 || fd > INT_MAX)
			continue;
		struct stat st;
		if (fstat((int)fd, &st) == 0 && st.st_dev == parent.st_dev &&
		    st.st_ino == parent.st_ino) {
			closedir(d);
			fprintf(stderr,
				"parent log descriptor survived exec\n");
			return 2;
		}
	}
	CHECK(closedir(d) == 0);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "fork-child.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 128;
	CHECK(logger_init(&c) == 0);
	LOG_INFO("child initialized after exec");
	CHECK(logger_flush_status() == 0);
	logger_shutdown();
	return 0;
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--child"))
		return exec_child();
	CHECK(argc == 1);
	unlink("fork-parent.log");
	unlink("fork-child.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "fork-parent.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 128;
	CHECK(logger_init(&c) == 0);
	LOG_INFO("before fork");
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1);
	exe[n] = 0;
	char *const args[] = { exe, "--child", NULL };
	CHECK(logger_prepare_fork() == 0); /* optional compatibility marker */
	pid_t p = fork();
	if (p < 0) {
		logger_after_fork_parent();
		return 3;
	}
	if (!p) {
		/* No malloc, stdio, logger reset/init or thread creation before exec. */
		execve(exe, args, environ);
		static const char message[] = "execve failed\n";
		(void)write(STDERR_FILENO, message, sizeof(message) - 1);
		_exit(127);
	}
	logger_after_fork_parent();
	LOG_INFO("parent continues");
	int status;
	while (waitpid(p, &status, 0) < 0)
		CHECK(errno == EINTR);
	CHECK(logger_flush_status() == 0);
	logger_shutdown();
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(contains("fork-parent.log", "before fork"));
	CHECK(contains("fork-parent.log", "parent continues"));
	CHECK(!contains("fork-parent.log", "child initialized"));
	CHECK(contains("fork-child.log", "child initialized after exec"));
	puts("fork+exec: child fresh runtime, parent output and CLOEXEC verified");
	return 0;
}
