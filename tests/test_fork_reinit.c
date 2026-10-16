#define _GNU_SOURCE
#include "logger.h"
#include "console.h"
#include "regression/support.h"
#include <sys/wait.h>

/* 黑盒用例只使用公开头和生产库。父、子文件由精确标记验收，
 * 不把初始化返回零或无崩溃当作日志内容正确。
 */
static void child_check(int ok, int code)
{
	if (!ok)
		_exit(code);
}

static void wait_ok(pid_t pid)
{
	int status;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		fprintf(stderr, "CHILD_STATUS=%d\n", status);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static logger_config_t config(const char *path, int async)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = path;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 8;
	cfg.async_mode = async;
	cfg.detail = LOGGER_DETAIL_DEBUG;
	return cfg;
}

static int lines_matching(const char *path, const char *marker)
{
	FILE *f = fopen(path, "r");
	CHECK(f);
	char line[8192];
	int count = 0;
	while (fgets(line, sizeof(line), f))
		count += strstr(line, marker) != NULL;
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return count;
}

static void child_run(int async, int global, int index, int conflict)
{
	errno = EDOM;
	child_check(logger_process_init() == 0 && errno == EDOM, 10);
	child_check(logger_process_init() == -EALREADY, 11);
	logger_context_t context = logger_context_get();
	child_check(!context.request_id && !context.session_id &&
			    !context.trace_id,
		    12);
	logger_config_t cfg = config("child.log", async);
	if (conflict) {
		logger_config_t taken = config("parent.log", async);
		child_check(logger_create(&taken, &(logger_t *){ NULL }) ==
				    -EBUSY,
			    13);
	}
	if (global) {
		logger_t *owner = NULL;
		child_check(logger_create(&cfg, &owner) == 0, 14);
		child_check(logger_default_set(NULL, owner) == 0, 14);
		LOG_INFO("CHILD_ONLY=%d", index);
		child_check(logger_flush_status() == 0, 15);
		child_check(logger_default_set(owner, NULL) == 0, 16);
		child_check(logger_destroy(&owner) == 0, 16);
	} else {
		logger_t *child = NULL;
		(void)logger_create(&cfg, &child);
		child_check(child != NULL, 17);
		LOGGER_INFO(child, "child", "CHILD_ONLY=%d", index);
		child_check(logger_flush_instance_status(child) == 0, 18);
		child_check(logger_destroy(&child) == 0, 19);
	}
	/* 第一个实例结束后创建另一个，不重置进程域。 */
	logger_config_t second_cfg = config("second.log", 0);
	logger_t *second = NULL;
	(void)logger_create(&second_cfg, &second);
	child_check(second != NULL, 20);
	LOGGER_INFO(second, "second", "SECOND_ONLY=%d", index);
	child_check(logger_destroy(&second) == 0, 21);
	child_check(console_print("CONSOLE_CHILD=%d\n", index) == 0, 22);
	_exit(0);
}

static void normal(const char *mode)
{
	int async = strcmp(mode, "sync") != 0;
	int global = !strcmp(mode, "global");
	int shut = !strcmp(mode, "shutdown-first");
	int count = !strcmp(mode, "repeated") ? 24 : 1;
	CHECK(logger_process_init() == 0);
	logger_context_set(
		&(logger_context_t){ .request_id = "PARENT_SECRET_CONTEXT" });
	console_set_verbosity(CONSOLE_QUIET);
	logger_config_t cfg = config("parent.log", async);
	logger_t *parent = NULL;
	CHECK(logger_create(&cfg, &parent) == 0 && parent != NULL);
	if (global)
		CHECK(logger_default_set(NULL, parent) == 0);
	if (global)
		LOG_INFO("PARENT_BEFORE");
	else
		LOGGER_INFO(parent, "parent", "PARENT_BEFORE");
	if (shut) {
		CHECK(logger_destroy(&parent) == 0);
		parent = NULL;
	}
	for (int i = 0; i < count; ++i) {
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			/* 未初始化时仍先于旧指针解引用拒绝；初始化后旧指针失效，
			 * 绝不测试“再次使用被释放指针可以可靠诊断”。
			 */
			errno = 0;
			LOGGER_INFO(parent, "old", "MUST_NOT_APPEAR");
			child_check(errno == ECHILD, 23);
			child_run(async, global, i, !shut);
		}
		wait_ok(pid);
	}
	CHECK(logger_context_get().request_id != NULL);
	CHECK(!strcmp(logger_context_get().request_id,
		      "PARENT_SECRET_CONTEXT"));
	if (global) {
		LOG_INFO("PARENT_AFTER");
		CHECK(logger_default_set(parent, NULL) == 0);
		CHECK(logger_destroy(&parent) == 0);
	} else if (parent) {
		LOGGER_INFO(parent, "parent", "PARENT_AFTER");
		CHECK(logger_destroy(&parent) == 0);
	}
	CHECK(lines_matching("parent.log", "PARENT_BEFORE") == 1);
	CHECK(lines_matching("parent.log", "PARENT_AFTER") == !shut);
	CHECK(lines_matching("parent.log", "CHILD_ONLY") == 0);
	CHECK(lines_matching("child.log", "CHILD_ONLY") == count);
	CHECK(lines_matching("child.log", "PARENT_") == 0);
	CHECK(lines_matching("child.log", "MUST_NOT_APPEAR") == 0);
	CHECK(lines_matching("second.log", "SECOND_ONLY") == count);
}

static int nested_child(unsigned depth)
{
	child_check(logger_process_init() == 0, 30);
	char path[64];
	child_check(snprintf(path, sizeof(path), "depth-%u.log", depth) > 0,
		    31);
	logger_config_t cfg = config(path, 1);
	logger_t *l = NULL;
	(void)logger_create(&cfg, &l);
	child_check(l != NULL, 32);
	LOGGER_INFO(l, "nested", "DEPTH=%u", depth);
	if (depth < 4) {
		pid_t pid = fork();
		child_check(pid >= 0, 33);
		if (!pid)
			_exit(nested_child(depth + 1));
		wait_ok(pid);
	}
	child_check(logger_destroy(&l) == 0, 34);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
	if (strcmp(argv[1], "sync")) {
		printf("FORK_REINIT_SKIP=tsan-post-fork-worker case=%s\n",
		       argv[1]);
		return 77;
	}
#endif
	char path[] = "/tmp/logger-fork-reinit-XXXXXX";
	enter_temp(path);
	if (!strcmp(argv[1], "nested")) {
		CHECK(logger_process_init() == 0);
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (!pid)
			_exit(nested_child(1));
		wait_ok(pid);
		for (unsigned i = 1; i <= 4; ++i) {
			char name[64];
			CHECK(snprintf(name, sizeof(name), "depth-%u.log", i) >
			      0);
			CHECK(lines_matching(name, "DEPTH=") == 1);
		}
	} else {
		CHECK(!strcmp(argv[1], "sync") || !strcmp(argv[1], "async") ||
		      !strcmp(argv[1], "global") ||
		      !strcmp(argv[1], "shutdown-first") ||
		      !strcmp(argv[1], "repeated"));
		normal(argv[1]);
	}
	const char *files[] = { "parent.log",  "child.log",   "second.log",
				"depth-1.log", "depth-2.log", "depth-3.log",
				"depth-4.log" };
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
		CHECK(unlink(files[i]) == 0 || errno == ENOENT);
		char lock[96];
		CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", files[i]) >
		      0);
		CHECK(unlink(lock) == 0 || errno == ENOENT);
	}
	CHECK(chdir("/") == 0);
	CHECK(rmdir(path) == 0);
	printf("FORK_REINIT_PUBLIC_PASS=%s\n", argv[1]);
	return 0;
}
