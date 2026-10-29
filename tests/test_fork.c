#define _GNU_SOURCE
#include <stdbool.h>
#include <errno.h>
#include "regression/default_fixture.h"
static logger_t *default_owner;
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

static bool check(bool condition, const char *expression, int line)
{
	(void)line;
	if (!condition)
		perror(expression);
	return condition;
}

#define CHECK(x) check((x), #x, __LINE__)
static bool contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	bool ok = !ferror(f) && strstr(b, needle) != NULL;
	fclose(f);
	return ok;
}
static int exec_child(void)
{
	/* 新程序映像可以使用完整 API。父进程日志 fd 不得跨 exec 存活。 */
	struct stat parent;
	if (!CHECK(stat("fork-parent.log", &parent) == 0))
		return 1;
	DIR *d = opendir("/proc/self/fd");
	if (!CHECK(d))
		return 1;
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
	if (!CHECK(closedir(d) == 0))
		return 1;
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "fork-child.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 128;
	if (!CHECK(fixture_default_create(&c, &default_owner) == 0))
		return 1;
	LOG_INFO("child initialized after exec");
	if (!CHECK(logger_default_flush() == 0))
		return 1;
	fixture_default_destroy(&default_owner);
	return 0;
}
int main(int argc, char **argv)
{
	if (!CHECK(logger_process_init() == 0))
		return 1;
	if (argc == 2 && !strcmp(argv[1], "--child"))
		return exec_child();
	if (!CHECK(argc == 1))
		return 1;
	unlink("fork-parent.log");
	unlink("fork-child.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "fork-parent.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 128;
	if (!CHECK(fixture_default_create(&c, &default_owner) == 0))
		return 1;
	LOG_INFO("before fork");
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (!CHECK(n > 0 && (size_t)n < sizeof(exe) - 1))
		return 1;
	exe[n] = 0;
	char *const args[] = { exe, "--child", NULL };
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (!p) {
		/*
		 * exec 前不使用 malloc、stdio、logger 重置或初始化，也不创建线
		 * 程。
		 */
		execve(exe, args, environ);
		static const char message[] = "execve failed\n";
		ssize_t ignored =
			write(STDERR_FILENO, message, sizeof(message) - 1);
		(void)ignored;
		_exit(127);
	}
	LOG_INFO("parent continues");
	int status;
	while (waitpid(p, &status, 0) < 0)
		if (!CHECK(errno == EINTR))
			return 1;
	if (!CHECK(logger_default_flush() == 0))
		return 1;
	fixture_default_destroy(&default_owner);
	if (!CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		return 1;
	if (!CHECK(contains("fork-parent.log", "before fork")))
		return 1;
	if (!CHECK(contains("fork-parent.log", "parent continues")))
		return 1;
	if (!CHECK(!contains("fork-parent.log", "child initialized")))
		return 1;
	if (!CHECK(contains("fork-child.log", "child initialized after exec")))
		return 1;
	puts("fork+exec: child fresh runtime, parent output and CLOEXEC verified");
	return 0;
}
