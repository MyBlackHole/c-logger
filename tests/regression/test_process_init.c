#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_process.h"
#include <sched.h>
#include <sys/wait.h>

/* 只包装进程引导的真实 libc 边界，不访问或伪造私有票据。 */
static _Atomic int registration_error;
static _Atomic int registration_calls;
static _Atomic int pause_before;
static _Atomic int pause_after;
static _Atomic int entered;
static _Atomic int release_registration;
static _Atomic int waiter_entered;
static _Atomic int waiter_done;
static _Atomic int inject_reentry;
static _Atomic int disable_error;
static _Atomic int restore_error;
static _Atomic int strict_child;
static int init_result;
static _Thread_local int observing_wait;

int __real_pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int __real_pthread_setcancelstate(int, int *);
int __real_sched_yield(void);

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (atomic_load(&strict_child))
		_exit(90);
	if (state == PTHREAD_CANCEL_DISABLE &&
	    atomic_exchange(&disable_error, 0))
		return EIO;
	int rc = __real_pthread_setcancelstate(state, old);
	if (!rc && state == PTHREAD_CANCEL_ENABLE &&
	    atomic_exchange(&restore_error, 0))
		return EIO;
	return rc;
}

int __wrap_sched_yield(void)
{
	if (atomic_load(&strict_child))
		_exit(91);
	if (observing_wait)
		atomic_store(&waiter_entered, 1);
	return __real_sched_yield();
}

static void pause_registration(void)
{
	atomic_store(&entered, 1);
	wait_flag(&release_registration);
}

int __wrap_pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void))
{
	/* Sanitizer 可能在 main 前自行注册，不能把其回调计入 Logger。 */
	if (c != logger_process_invalidate_child)
		return __real_pthread_atfork(a, b, c);
	if (atomic_load(&strict_child))
		_exit(92);
	atomic_fetch_add(&registration_calls, 1);
	if (atomic_exchange(&inject_reentry, 0)) {
		CHECK(logger_process_init() == -EDEADLK);
		CHECK(logger_process_ensure() == -EDEADLK);
	}
	if (atomic_load(&pause_before))
		pause_registration();
	int error = atomic_load(&registration_error);
	if (error)
		return error;
	int rc = __real_pthread_atfork(a, b, c);
	if (atomic_load(&pause_after))
		pause_registration();
	return rc;
}

static void *initializer(void *unused)
{
	(void)unused;
	errno = EDOM;
	init_result = logger_process_init();
	CHECK(errno == EDOM);
	/* 恢复延迟取消状态本身不必是取消点；由调用方显式到达取消点。 */
	pthread_testcancel();
	return NULL;
}

static void *ordinary_waiter(void *unused)
{
	(void)unused;
	observing_wait = 1;
	CHECK(logger_process_ensure() == 0);
	observing_wait = 0;
	atomic_store(&waiter_done, 1);
	return NULL;
}

static void ready_works(void)
{
	errno = ERANGE;
	CHECK(logger_process_init() == -EALREADY && errno == ERANGE);
	CHECK(logger_process_ensure() == 0);
	CHECK(logger_process_object_count() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.async_mode = 0;
	logger_t *l = logger_create(&cfg);
	CHECK(l);
	LOGGER_INFO(l, "init", "READY_AFTER_TRANSITION");
	CHECK(logger_destroy_status(l) == 0);
	CHECK(logger_process_object_count() == 0);
}

static void failed_registration(void)
{
	atomic_store(&registration_error, ENOMEM);
	errno = EDOM;
	CHECK(logger_process_init() == -ENOMEM && errno == EDOM);
	CHECK(logger_process_ensure() == -ENOMEM);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	CHECK(logger_create(&cfg) == NULL && errno == ENOMEM);
	CHECK(atomic_load(&registration_calls) == 1);
	CHECK(logger_process_object_count() == 0);
}

static void retry_errors(int twice)
{
	failed_registration();
	if (twice) {
		atomic_store(&registration_error, EAGAIN);
		errno = EDOM;
		CHECK(logger_process_init() == -EAGAIN && errno == EDOM);
		CHECK(logger_process_ensure() == -EAGAIN);
	}
	atomic_store(&registration_error, 0);
	CHECK(logger_process_init() == 0);
	ready_works();
	CHECK(atomic_load(&registration_calls) == (twice ? 3 : 2));
}

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

static void child_reject(void)
{
	atomic_store(&strict_child, 1);
	errno = EDOM;
	if (logger_process_init() != -ECHILD || errno != EDOM)
		_exit(30);
	if (logger_process_ensure() != -ECHILD)
		_exit(31);
}

static void fork_reject(void)
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		child_reject();
		_exit(0);
	}
	wait_ok(pid);
}

static void in_progress(const char *mode)
{
	int retry = !strcmp(mode, "retry-concurrent");
	int concurrent = retry || !strcmp(mode, "concurrent");
	int cancel = !strcmp(mode, "cancel-init");
	int after = !strcmp(mode, "fork-installed");
	if (retry) {
		failed_registration();
		atomic_store(&registration_error, 0);
	}
	atomic_store(after ? &pause_after : &pause_before, 1);
	pthread_t init_thread;
	CHECK(pthread_create(&init_thread, NULL, initializer, NULL) == 0);
	wait_flag(&entered);
	errno = ERANGE;
	CHECK(logger_process_init() == -EAGAIN && errno == ERANGE);
	CHECK(logger_process_object_count() == 0);

	pthread_t waiter;
	if (concurrent) {
		CHECK(!pthread_create(&waiter, NULL, ordinary_waiter, NULL));
		wait_flag(&waiter_entered);
		CHECK(!atomic_load(&waiter_done));
	} else if (cancel) {
		CHECK(pthread_cancel(init_thread) == 0);
	} else {
		fork_reject();
	}
	atomic_store(&release_registration, 1);
	void *result;
	CHECK(pthread_join(init_thread, &result) == 0);
	if (cancel)
		CHECK(result == PTHREAD_CANCELED && init_result == 0);
	else
		CHECK(!result && init_result == 0);
	if (concurrent) {
		CHECK(pthread_join(waiter, NULL) == 0);
		CHECK(atomic_load(&waiter_done));
	}
	ready_works();
	CHECK(atomic_load(&registration_calls) == (retry ? 2 : 1));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *mode = argv[1];
	if (!strcmp(mode, "register-error")) {
		retry_errors(0);
	} else if (!strcmp(mode, "repeat-errors")) {
		retry_errors(1);
	} else if (!strcmp(mode, "reentrant")) {
		atomic_store(&inject_reentry, 1);
		CHECK(logger_process_init() == 0);
		ready_works();
		CHECK(atomic_load(&registration_calls) == 1);
	} else if (!strcmp(mode, "earlier-handler")) {
		CHECK(__real_pthread_atfork(NULL, NULL, child_reject) == 0);
		CHECK(logger_process_init() == 0);
		fork_reject();
		ready_works();
	} else if (!strcmp(mode, "disable-error")) {
		atomic_store(&disable_error, 1);
		errno = EDOM;
		CHECK(logger_process_init() == -EIO && errno == EDOM);
		CHECK(atomic_load(&registration_calls) == 0);
		CHECK(logger_process_init() == 0);
		ready_works();
	} else if (!strcmp(mode, "restore-error")) {
		atomic_store(&restore_error, 1);
		CHECK(logger_process_init() == -EIO);
		ready_works();
		CHECK(atomic_load(&registration_calls) == 1);
	} else if (!strcmp(mode, "fork-failed-state")) {
		failed_registration();
		fork_reject();
		atomic_store(&registration_error, 0);
		CHECK(logger_process_init() == 0);
		ready_works();
	} else {
		CHECK(!strcmp(mode, "concurrent") ||
		      !strcmp(mode, "retry-concurrent") ||
		      !strcmp(mode, "fork-window") ||
		      !strcmp(mode, "fork-installed") ||
		      !strcmp(mode, "cancel-init"));
		in_progress(mode);
	}
	printf("PROCESS_BOOTSTRAP_PASS=%s\n", mode);
	return 0;
}
