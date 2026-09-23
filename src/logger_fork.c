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

	/* pthread_join may observe the cleared TID just before the kernel removes
     * an exiting task from procfs. Allow that bounded cleanup delay only. This
     * is not an attempt to shut down arbitrary application/library threads. */
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
	/* All application atfork handlers must preserve the quiescent boundary:
     * no thread creation or logger/audit/console reinitialization inside them. */
	pid = fork();
	int fork_error = pid < 0 ? errno : 0;
	rc = logger_process_finish_clean_fork();
	if (rc) {
		/* An invalid child must not return into the application as though it
         * had a clean runtime. This path is only reachable on a broken contract. */
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
