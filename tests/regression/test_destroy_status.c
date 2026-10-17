#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include <fcntl.h>
static int fail_close, fail_join, fail_cond_destroy, fail_mutex_destroy;
static int selected_fd = -1;
static int selected_close_calls;
static logger_t *subject;
static int subject_freed;

/* 只在仍然有效的对象上注入 destroy 失败，不对活跃同步对象调用
 * 真正的 pthread_*_destroy。额外检查最终 logger_t 是否发生 free。
 */
int __real_logger_runtime_free(void *);
int __wrap_logger_runtime_free(void *p)
{
	if (subject && p == subject) {
		++subject_freed;
		subject = NULL;
	}
	return __real_logger_runtime_free(p);
}

int __real_close(int);
int __wrap_close(int fd)
{
	int selected = fd == selected_fd && selected_fd >= 0;
	if (selected)
		++selected_close_calls;
	/* 先由内核真正关闭 fd，再返回模拟的 EINTR / EIO。
	 * Linux close(EINTR) 后 fd 可能已被复用，不可重试此整数。
	 */
	int rc = __real_close(fd);
	if (selected && fail_close) {
		int error = fail_close;
		fail_close = 0;
		errno = error;
		return -1;
	}
	return rc;
}

int __real_pthread_join(pthread_t, void **);
int __wrap_pthread_join(pthread_t thread, void **result)
{
	if (fail_join) {
		fail_join = 0;
		/* 让真实工作线程静止，使测试自身没有失控线程，
		 * 但向实现报告生命周期证明失败。实现必须保守地拒绝最终 free，
		 * 因为它不能依赖失败的 pthread_join() 背后隐藏的事实。
		 */
		int rc = __real_pthread_join(thread, result);
		return rc ? rc : EDEADLK;
	}
	return __real_pthread_join(thread, result);
}

int __real_pthread_cond_destroy(pthread_cond_t *);
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (subject && cv == &subject->progress_cv && fail_cond_destroy) {
		fail_cond_destroy = 0;
		return EBUSY;
	}
	return __real_pthread_cond_destroy(cv);
}

int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __wrap_pthread_mutex_destroy(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->progress_mu && fail_mutex_destroy) {
		fail_mutex_destroy = 0;
		return EBUSY;
	}
	return __real_pthread_mutex_destroy(mu);
}
int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	unlink("dispose.log");
	unlink("next.log");
	if (!strcmp(argv[1], "null")) {
		CHECK(logger_destroy(&(logger_t *){ NULL }) == 0);
		return 0;
	}
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "dispose.log";
	c.async_mode = 1;
	c.queue_capacity = 2048;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	int io_error = !strcmp(argv[1], "io-error");
	if (io_error) {
		c.file_path = "/dev/full";
		c.async_mode = 0;
	}
	logger_t *l = NULL;
	CHECK(logger_create(&c, &l) == 0);
	CHECK(l != NULL);
	subject = l;
	if (!strcmp(argv[1], "join-error"))
		fail_join = 1;
	else if (!strcmp(argv[1], "cond-destroy-error"))
		fail_cond_destroy = 1;
	else if (!strcmp(argv[1], "mutex-destroy-error"))
		fail_mutex_destroy = 1;
	if (!strcmp(argv[1], "close-error") ||
	    !strcmp(argv[1], "close-eintr")) {
		struct logger_borrowed_fd view =
			logger_fd_borrow(&l->file_backend.fd);
		selected_fd = logger_fd_number(view);
		fail_close = !strcmp(argv[1], "close-eintr") ? EINTR : EIO;
	}
	int lifecycle_error = fail_join || fail_cond_destroy || fail_mutex_destroy;
	int expected = 0;
	if (io_error)
		expected = ENOSPC;
	else if (fail_close)
		expected = fail_close;
	else if (fail_join)
		expected = EDEADLK;
	else if (fail_cond_destroy || fail_mutex_destroy)
		expected = EBUSY;
	for (int i = 0; i < (io_error ? 1 : 1000); ++i)
		LOGGER_INFO(l, "test", "record=%d", i);
	errno = EDOM;
	int rc = logger_destroy(&l);
	CHECK(l == NULL); /* 消费发生在接口内部，不由调用者补写 NULL。 */
	CHECK(rc == -expected && errno == EDOM);
	if (lifecycle_error) {
		/* 失败注入在对象已合法静止后直接返回 EBUSY；测试不依赖
		 * 对仍有使用者的 pthread 对象执行 destroy 的未定义行为。
		 */
		CHECK(subject_freed == 0);
		CHECK(logger_process_object_count() == 1);
		return 0;
	}
	CHECK(subject_freed == 1);
	CHECK(logger_process_object_count() == 0);
	if (selected_fd >= 0) {
		CHECK(selected_close_calls == 1);
		CHECK(fcntl(selected_fd, F_GETFD) == -1 && errno == EBADF);
	}
	if (!io_error)
		CHECK(log_contains("dispose.log", "record=999"));
	subject = NULL;
	c.file_path = "next.log";
	c.async_mode = 0;
	CHECK(logger_create(&c, &l) == 0);
	CHECK(l != NULL);
	CHECK(logger_destroy(&l) == 0);
	return 0;
}
