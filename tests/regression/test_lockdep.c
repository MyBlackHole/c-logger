#define _POSIX_C_SOURCE 200809L
#include "logger_lockdep.h"
#include "support.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int a, b, c;

static void expect_abort(void (*fn)(void))
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		fn();
		_exit(0);
	}
	int status;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
}

static void bad_instance_nesting(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_PROGRESS, &b);
}

static void bad_reverse_global(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
}

static void bad_release(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);
}

static void bad_assert(void)
{
	logger_lockdep_assert_held(LOGGER_LOCK_QUEUE_WAIT, &a);
}

int main(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_CONTROL, &a);

	logger_lockdep_acquire(LOGGER_LOCK_AUDIT_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_AUDIT_OPERATION, &b);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_PROGRESS, &c);
	logger_lockdep_release(LOGGER_LOCK_INSTANCE_PROGRESS, &c);
	logger_lockdep_release(LOGGER_LOCK_AUDIT_OPERATION, &b);
	logger_lockdep_release(LOGGER_LOCK_AUDIT_CONTROL, &a);

	expect_abort(bad_instance_nesting);
	expect_abort(bad_reverse_global);
	expect_abort(bad_release);
	expect_abort(bad_assert);

	return 0;
}
