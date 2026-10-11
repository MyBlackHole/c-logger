#ifndef LOGGER_SIGPIPE_H
#define LOGGER_SIGPIPE_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

/* 为 write/writev 和 stdio 管道失败提供私有、线程局部的保护。
 * 调用者必须将取消推迟到 end()，包括释放自己的锁之后。
 * 绝不改变进程级信号处置，也不消耗进入时已待处理的 SIGPIPE。
 * 成功设置后保留 printf %m 的语义。
 */
typedef struct {
	sigset_t old_mask;
	int was_pending;
} logger_sigpipe_guard_t;

static inline int logger_sigpipe_begin(logger_sigpipe_guard_t *guard)
{
	int saved_errno = errno;
	sigset_t block, pending;
	if (sigemptyset(&block) != 0 || sigaddset(&block, SIGPIPE) != 0)
		return -(errno ? errno : EINVAL);
	int rc = pthread_sigmask(SIG_BLOCK, &block, &guard->old_mask);
	if (rc != 0)
		return -rc;
	if (sigpending(&pending) != 0) {
		int error = errno ? errno : EIO;
		(void)pthread_sigmask(SIG_SETMASK, &guard->old_mask, NULL);
		return -error;
	}
	int member = sigismember(&pending, SIGPIPE);
	if (member < 0) {
		int error = errno ? errno : EINVAL;
		(void)pthread_sigmask(SIG_SETMASK, &guard->old_mask, NULL);
		return -error;
	}
	guard->was_pending = member;
	errno = saved_errno;
	return 0;
}

static inline void logger_sigpipe_consume_new(int was_pending)
{
	if (was_pending)
		return;
	sigset_t block, pending;
	if (sigemptyset(&block) != 0 || sigaddset(&block, SIGPIPE) != 0)
		return;
	if (sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) != 1)
		return;
	struct timespec timeout = { 0 };
	for (;;) {
		int rc = sigtimedwait(&block, NULL, &timeout);
		if (rc == SIGPIPE || (rc < 0 && errno == EAGAIN))
			return;
		if (rc < 0 && errno == EINTR)
			continue;
		return;
	}
}

/* saw_epipe 与首个错误分开记录：格式化因其他原因失败后，
 * 后续 fflush 仍可能因 EPIPE 失败。
 */
static inline int logger_sigpipe_end(logger_sigpipe_guard_t *guard,
				     int result, int saw_epipe)
{
	int saved_errno = errno;
	if (saw_epipe)
		logger_sigpipe_consume_new(guard->was_pending);
	int restore = pthread_sigmask(SIG_SETMASK, &guard->old_mask, NULL);
	errno = saved_errno;
	return !result && restore ? -restore : result;
}

#endif
