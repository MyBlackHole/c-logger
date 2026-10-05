#include "logger_scope.h"
#include "logger_process.h"
#include <errno.h>
#include <pthread.h>

static _Thread_local int in_runtime;

int logger_scope_busy(void)
{
	return in_runtime;
}

int logger_scope_begin(logger_scope_t *scope)
{
	/* 首先不接触任何继承锁、实例指针或注册状态。 */
	if (logger_process_is_child()) {
		errno = ECHILD;
		return -ECHILD;
	}
	if (in_runtime) {
		errno = EDEADLK;
		return -EDEADLK;
	}
	scope->saved_errno = errno;
	int rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					&scope->old_cancel);
	if (rc) {
		errno = rc;
		return -rc;
	}
	in_runtime = 1;
	rc = logger_process_ensure();
	if (rc)
		return logger_scope_end(scope, rc);
	return 0;
}

int logger_scope_end(logger_scope_t *scope, int rc)
{
	/* 必须在 va_end、互斥锁释放、发布以及全部资源清理之后调用。
     * 该函数之后不得再解引用 Logger：取消恢复后，调用方的取消清理处理器
     * 可能立即释放其拥有的实例。 */
	int error = rc < 0 ? -rc : scope->saved_errno;
	in_runtime = 0;
	errno = error;
	(void)pthread_setcancelstate(scope->old_cancel, NULL);
	errno = error;
	return rc;
}

void logger_scope_worker_enter(void)
{
	in_runtime = 1;
}
void logger_scope_worker_leave(void)
{
	in_runtime = 0;
}
