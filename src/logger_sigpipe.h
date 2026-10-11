#ifndef LOGGER_SIGPIPE_H
#define LOGGER_SIGPIPE_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

/* Private, thread-local protection for write/writev and stdio pipe failures.
 * The caller must defer cancellation until end(), including after releasing
 * its locks. Never change the process-wide disposition or consume a SIGPIPE
 * that was already pending on entry. Successful setup preserves printf %m.
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

/* saw_epipe is separate from the first error: a later fflush can fail with
 * EPIPE after formatting has already failed for a different reason. */
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
