#define _GNU_SOURCE
#include "logger_internal.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

pid_t logger_fork_reinit(void)
{
	int rc = logger_process_ensure();
	if (rc) {
		errno = -rc;
		return -1;
	}
	int old_cancel;
	rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
	if (rc) {
		errno = rc;
		return -1;
	}
	pid_t pid = -1;
	rc = logger_global_stop_for_clean_fork();
	if (rc)
		goto done;

	/* pthread_join 可能在内核从 procfs 移除退出任务前观察到已清零的 TID。
     * 这里只允许这种有界的清理延迟，并不尝试关闭任意应用或库线程。 */
	for (unsigned retry = 0;; ++retry) {
		unsigned threads;
		rc = logger_process_thread_count(&threads);
		if (rc)
			goto done;
		if (threads == 1)
			break;
		if (retry == 50) {
			rc = -EBUSY;
			goto done;
		}
		struct timespec delay = { .tv_nsec = 1000000 };
		(void)nanosleep(&delay, NULL);
	}
	rc = logger_process_arm_clean_fork();
	if (rc)
		goto done;
	/* 所有应用侧 atfork 处理器都必须保持静默边界：其中不得创建线程，
     * 也不得重新初始化 Logger/Audit/Console。 */
	pid = fork();
	int fork_error = pid < 0 ? errno : 0;
	rc = logger_process_finish_clean_fork();
	if (rc) {
		/* 非法子进程不能像拥有干净运行时一样返回应用层。
         * 只有调用约定被破坏时才会到达这条路径。 */
		if (pid == 0)
			_exit(127);
		goto done;
	}
	if (pid == 0)
		logger_context_clear();
	if (fork_error)
		rc = -fork_error;
done:
	(void)pthread_setcancelstate(old_cancel, NULL);
	if (rc) {
		errno = -rc;
		return -1;
	}
	return pid;
}
