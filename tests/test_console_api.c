#define _GNU_SOURCE
#include "logger.h"
#include "console.h"
#include "regression/support.h"
#include <fcntl.h>
#include <sys/wait.h>

/* 只使用公开接口与真实 FILE/终端，不链接私有入口或故障包装器。 */
static void result(int rc, int error)
{
	CHECK(rc == -error && errno == EDOM);
}

static void reject_all(int error)
{
	console_config_t cfg = CONSOLE_DEFAULT_CONFIG();
	int tty = 7;
	int count = -1;
	errno = EDOM;
	result(console_init(&cfg), error);
	result(console_set_color(CONSOLE_COLOR_NEVER), error);
	result(console_set_verbosity(CONSOLE_QUIET), error);
	result(console_stdout_is_tty(&tty), error);
	result(console_stderr_is_tty(&tty), error);
	CHECK(tty == 7);
	result(console_print("forbidden%n", &count), error);
	result(console_info("forbidden%n", &count), error);
	result(console_warn("forbidden%n", &count), error);
	result(console_error("forbidden%n", &count), error);
	result(console_verbose("forbidden%n", &count), error);
	result(console_debug("forbidden%n", &count), error);
	result(console_debug_source(__FILE__, __LINE__, __func__, "forbidden%n",
				    &count),
	       error);
	CHECK(count == -1);
	/* 准入先于读取调用者内存，不依赖数据指针本身可解引用。 */
	result(console_init((const console_config_t *)(uintptr_t)1), error);
	result(console_stdout_is_tty((int *)(uintptr_t)1), error);
}

static void config_and_output(void)
{
	FILE *sink = tmpfile();
	CHECK(sink);
	FILE *old_out = stdout;
	FILE *old_err = stderr;
	stdout = stderr = sink;
	console_config_t cfg = CONSOLE_DEFAULT_CONFIG();
	cfg.verbosity = CONSOLE_QUIET;
	cfg.color = CONSOLE_COLOR_NEVER;
	errno = EDOM;
	result(console_init(&cfg), 0);
	result(console_init(NULL), EINVAL);
	result(console_set_verbosity((console_verbosity_t)-1), EINVAL);
	result(console_set_color((console_color_mode_t)-1), EINVAL);
	console_config_t bad = cfg;
	bad.verbosity = CONSOLE_DEBUG;
	bad.color = (console_color_mode_t)-1;
	result(console_init(&bad), EINVAL);
	result(console_info("hidden"), 0);
	result(console_verbose("hidden"), 0);
	result(console_debug("hidden"), 0);
	int (*outputs[])(const char *, ...) = {
		console_print, console_info,	console_warn,
		console_error, console_verbose, console_debug
	};
	for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i)
		result(outputs[i](NULL), EINVAL);
	result(console_debug_source(NULL, 0, NULL, NULL), EINVAL);
	result(console_warn("one"), 0);
	result(console_set_verbosity(CONSOLE_DEBUG), 0);
	result(console_set_color(CONSOLE_COLOR_NEVER), 0);
	result(console_print("two\n"), 0);
	result(console_info("three"), 0);
	result(console_error("four"), 0);
	result(console_verbose("five"), 0);
	result(console_debug("six"), 0);
	result(console_debug_source("/tmp/source.c", 7, "caller", "seven"), 0);
	stdout = old_out;
	stderr = old_err;
	CHECK(fseek(sink, 0, SEEK_SET) == 0);
	char actual[256] = { 0 };
	const char expected[] =
		"WARN: one\ntwo\nINFO: three\nERROR: four\n"
		"INFO: five\nDEBUG: six\nDEBUG: source.c:7 caller() seven\n";
	CHECK(fread(actual, 1, sizeof(actual), sink) == sizeof(expected) - 1);
	CHECK(!strcmp(actual, expected));
	CHECK(fclose(sink) == 0);
}

static void tty_query(void)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	CHECK(master >= 0);
	CHECK(grantpt(master) == 0 && unlockpt(master) == 0);
	char *name = ptsname(master);
	CHECK(name);
	int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	int file = open("/dev/null", O_WRONLY | O_CLOEXEC);
	CHECK(slave >= 0 && file >= 0);
	int (*queries[])(int *) = { console_stdout_is_tty,
				    console_stderr_is_tty };
	for (int index = 0; index < 2; ++index) {
		int fd = index ? STDERR_FILENO : STDOUT_FILENO;
		int saved = dup(fd);
		CHECK(saved >= 0);
		CHECK(dup2(file, fd) == fd);
		int answer = 7;
		errno = EDOM;
		result(queries[index](&answer), 0);
		CHECK(answer == 0);
		result(queries[index](NULL), EINVAL);
		CHECK(dup2(slave, fd) == fd);
		errno = EDOM;
		result(queries[index](&answer), 0);
		CHECK(answer == 1);
		CHECK(close(fd) == 0);
		errno = EDOM;
		int rc = queries[index](&answer);
		int error = errno;
		CHECK(dup2(saved, fd) == fd);
		CHECK(close(saved) == 0);
		CHECK(rc == -EBADF && error == EDOM && answer == 1);
	}
	CHECK(close(file) == 0 && close(slave) == 0 && close(master) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "cold")) {
		reject_all(ENODEV);
		/* 所有失败调用都不能偷偷初始化进程域。 */
		CHECK(logger_process_init() == 0);
	} else {
		CHECK(logger_process_init() == 0);
		if (!strcmp(argv[1], "config-output")) {
			config_and_output();
		} else if (!strcmp(argv[1], "tty")) {
			tty_query();
		} else {
			CHECK(!strcmp(argv[1], "fork"));
			pid_t pid = fork();
			CHECK(pid >= 0);
			if (!pid) {
				reject_all(ECHILD);
				CHECK(logger_process_init() == 0);
				config_and_output();
				_exit(0);
			}
			int status;
			CHECK(waitpid(pid, &status, 0) == pid);
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			config_and_output();
		}
	}
	printf("CONSOLE_API_PASS=%s\n", argv[1]);
	return 0;
}
