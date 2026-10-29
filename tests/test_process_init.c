#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include "process_rejection.h"
#include <sys/wait.h>

static void wait_ok(pid_t pid)
{
	int status;
	pid_t got;

	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void cold(void)
{
	char path[] = "/tmp/logger-process-init-XXXXXX";
	enter_temp(path);
	errno = ERANGE;
	CHECK(logger_process_init() == 0 && errno == ERANGE);
	CHECK(logger_process_init() == -EALREADY && errno == ERANGE);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 8;
	logger_t *a = NULL;
	CHECK(logger_create(&cfg, &a) == 0);
	CHECK(a != NULL);
	logger_config_t bcfg = LOGGER_DEFAULT_CONFIG();
	bcfg.async_mode = 0;
	logger_t *b = NULL;
	CHECK(logger_create(&bcfg, &b) == 0);
	CHECK(b != NULL);
	LOGGER_INFO(a, "init", "BEFORE_FORK");
	CHECK(logger_flush(a) == 0);

	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		errno = EDOM;
		/* 继承资源隔离完成后建立新域；不复活旧实例。 */
		if (logger_process_init() != 0 || errno != EDOM)
			_exit(10);
		_exit(0);
	}
	wait_ok(pid);
	CHECK(logger_process_init() == -EALREADY);
	LOGGER_INFO(a, "init", "PARENT_CONTINUES");
	CHECK(logger_destroy(&b) == 0);
	CHECK(logger_destroy(&a) == 0);
	CHECK(file_contains("out.log", "BEFORE_FORK"));
	CHECK(file_contains("out.log", "PARENT_CONTINUES"));
	/* 对象计数归零不是全进程资源清理证明，不能据此放行子方。 */
	pid = fork();
	CHECK(pid >= 0);
	if (!pid)
		_exit(logger_process_init() == 0 ? 0 : 11);
	wait_ok(pid);
	leave_temp(path);
}

static void prefork(void)
{
	char path[] = "/tmp/logger-process-prefork-XXXXXX";
	enter_temp(path);
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		/* 本用例在首次使用库前、单线程下 fork，不模拟已初始化子方恢复。 */
		if (logger_process_init())
			_exit(20);
		logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
		cfg.outputs = LOGGER_OUT_FILE;
		cfg.file_path = "out.log";
		cfg.rotation.mode = LOGGER_ROTATE_NONE;
		cfg.queue_capacity = 8;
		logger_t *l = NULL;
		(void)logger_create(&cfg, &l);
		if (!l)
			_exit(21);
		LOGGER_INFO(l, "init", "CHILD_FIRST_INIT");
		_exit(logger_destroy(&l) ? 22 : 0);
	}
	wait_ok(pid);
	CHECK(logger_process_init() == 0);
	CHECK(file_contains("out.log", "CHILD_FIRST_INIT"));
	leave_temp(path);
}

static void uninitialized(void)
{
	/* 多轮普通调用不能变成隐式初始化；显式调用仍是首次成功。 */
	for (unsigned int i = 0; i < 3; ++i)
		check_process_rejection(ENODEV);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.async_mode = 0;
	logger_t *l = NULL;
	CHECK(logger_create(&cfg, &l) == 0);
	CHECK(l != NULL);
	CHECK(logger_process_init() == -EALREADY);
	CHECK(logger_destroy(&l) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "cold"))
		cold();
	else if (!strcmp(argv[1], "prefork"))
		prefork();
	else if (!strcmp(argv[1], "uninitialized"))
		uninitialized();
	else
		CHECK(0);
	printf("PROCESS_INIT_PASS=%s\n", argv[1]);
	return 0;
}
