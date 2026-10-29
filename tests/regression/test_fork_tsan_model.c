#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_runtime.h"
#include <sys/wait.h>

#if defined(__SANITIZE_THREAD__)
#define MODEL_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MODEL_TSAN 1
#endif
#endif /* defined(__SANITIZE_THREAD__) */

#ifdef MODEL_TSAN
static _Atomic int ready;
static _Atomic int go;
static volatile int intentionally_racy;

static void *race_writer(void *unused)
{
	(void)unused;
	atomic_fetch_add(&ready, 1);
	while (!atomic_load(&go))
		sched_yield();
	/* 仅诊断反例：必须由检测器拒绝，不链接到安装后的库。 */
	for (unsigned int i = 0; i < 10000; ++i)
		intentionally_racy = (int)i;
	return NULL;
}

static logger_t *bind(void)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.async_mode = 0;
	logger_t *owner = NULL;

	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	LOG_INFO("TSAN_MODEL_REAL_DEFAULT");
	return owner;
}

static void unbind(logger_t **owner)
{
	CHECK(logger_default_set(*owner, NULL) == 0);
	CHECK(logger_destroy(owner) == 0);
}

static void child_case(const char *mode)
{
	CHECK(logger_process_init() == 0);
	logger_t *owner = bind();

	if (!strcmp(mode, "order-error")) {
		struct logger_runtime *r = logger_runtime_current();
		/*
		 * 同一新域的真实协调锁：先建立正确边，再故意反转。
		 * 没有并发死锁，但检测器必须保留此实际 ABBA 诊断。
		 */
		CHECK(pthread_mutex_lock(&r->format_mu) == 0);
		CHECK(pthread_mutex_lock(&r->resource_mu) == 0);
		CHECK(pthread_mutex_unlock(&r->resource_mu) == 0);
		CHECK(pthread_mutex_unlock(&r->format_mu) == 0);
		CHECK(pthread_mutex_lock(&r->resource_mu) == 0);
		CHECK(pthread_mutex_lock(&r->format_mu) == 0);
		CHECK(pthread_mutex_unlock(&r->format_mu) == 0);
		CHECK(pthread_mutex_unlock(&r->resource_mu) == 0);
	} else if (!strcmp(mode, "race-error")) {
		pthread_t first;
		pthread_t second;

		CHECK(pthread_create(&first, NULL, race_writer, NULL) == 0);
		CHECK(pthread_create(&second, NULL, race_writer, NULL) == 0);
		while (atomic_load(&ready) != 2)
			sched_yield();
		atomic_store(&go, 1);
		CHECK(pthread_join(first, NULL) == 0);
		CHECK(pthread_join(second, NULL) == 0);
	} else {
		CHECK(!strcmp(mode, "healthy"));
	}
	unbind(&owner);
}
#endif /* MODEL_TSAN */

int main(int argc, char **argv)
{
#ifndef MODEL_TSAN
	(void)argc;
	(void)argv;
	/* 非 TSan 二进制绝不能作为三个反例的合格执行入口。 */
	fputs("TSAN_MODEL_REQUIRES_INSTRUMENTATION\n", stderr);
	return 77;
#else
	CHECK(argc == 2);
	alarm(20);
	CHECK(logger_process_init() == 0);
	logger_t *parent = bind();
	/* 父方只有调用线程；不得利用多线程 fork 的原生报告忽略模式。 */
	pid_t pid = fork();

	CHECK(pid >= 0);
	if (!pid) {
		child_case(argv[1]);
		_exit(0);
	}
	int status;

	CHECK(waitpid(pid, &status, 0) == pid);
	unbind(&parent);
	if (!WIFEXITED(status))
		return 99;
	return WEXITSTATUS(status);
#endif /* MODEL_TSAN */
}
