#define _GNU_SOURCE
#include "global_support.h"
#include <stdarg.h>

/* Cancellation probes pause with only test atomics, never a test mutex that
 * could itself be abandoned by cancellation. The real library lock/pin is the
 * resource under test. Both masks and post-cancel reuse are checked. */
enum point {
	P_NONE,
	P_CREATE,
	P_WRITE,
	P_REOPEN,
	P_DISPOSE,
	P_BOOTSTRAP,
	P_QUEUE,
	P_READER,
	P_CONTROL
};
static _Atomic int at_point, release_point, seen_disabled, stage, create_fails;
static _Atomic int hold_format, format_entered, release_format, flush_waiting;
static _Atomic int cleanup_seen, operation_returned;
static _Thread_local int delay_reader, delay_controller, watch_cond;
static int cleanup_phase_error;
static const char *scenario;

logger_t *__real_logger_create(const logger_config_t *);
int __real_logger_destroy_status(logger_t *);
int __real_logger_file_write(logger_file_t *, const char *, size_t,
			     const struct timespec *, int);
int __real_logger_file_reopen(logger_file_t *);
int __real_logger_queue_push(logger_queue_t *, const logger_message_t *);
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);

static void inspect_mask(void)
{
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	atomic_store(&seen_disabled, old == PTHREAD_CANCEL_DISABLE);
	CHECK(!pthread_setcancelstate(old, NULL));
}
static void rendezvous(enum point p)
{
	int expected = p;
	if (!atomic_compare_exchange_strong(&stage, &expected, P_NONE))
		return;
	inspect_mask();
	atomic_store(&at_point, 1);
	wait_flag(&release_point);
}
logger_t *__wrap_logger_create(const logger_config_t *cfg)
{
	rendezvous(P_CREATE);
	if (atomic_exchange(&create_fails, 0)) {
		errno = ENOSPC;
		return NULL;
	}
	return __real_logger_create(cfg);
}
int __wrap_logger_destroy_status(logger_t *l)
{
	rendezvous(P_DISPOSE);
	return __real_logger_destroy_status(l);
}
int __wrap_logger_file_write(logger_file_t *f, const char *text, size_t n,
			     const struct timespec *ts, int sync)
{
	rendezvous(P_WRITE);
	return __real_logger_file_write(f, text, n, ts, sync);
}
int __wrap_logger_file_reopen(logger_file_t *f)
{
	rendezvous(P_REOPEN);
	return __real_logger_file_reopen(f);
}
int __wrap_logger_queue_push(logger_queue_t *q, const logger_message_t *m)
{
	rendezvous(P_QUEUE);
	return __real_logger_queue_push(q, m);
}
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	if (atomic_exchange(&hold_format, 0)) {
		atomic_store(&format_entered, 1);
		wait_flag(&release_format);
	}
	return __real_logger_format_line(l, m, out, cap);
}
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (watch_cond) {
		watch_cond = 0;
		inspect_mask();
		atomic_store(&flush_waiting, 1);
	}
	return __real_pthread_cond_wait(cv, mu);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (delay_controller) {
		delay_controller = 0;
		rendezvous(P_CONTROL);
	}
	return __real_pthread_mutex_lock(mu);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (delay_reader) {
		delay_reader = 0;
		rendezvous(P_READER);
	}
	return __real_pthread_rwlock_rdlock(rw);
}
int __wrap_dprintf(int fd, const char *fmt, ...)
{
	rendezvous(P_BOOTSTRAP);
	va_list ap;
	va_start(ap, fmt);
	int rc = vdprintf(fd, fmt, ap);
	va_end(ap);
	return rc;
}
static void cleanup(void *unused)
{
	(void)unused;
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	logger_io_metrics_t io;
	errno = 0;
	logger_get_global_io_metrics(&io);
	CHECK(errno == cleanup_phase_error);
	if (!cleanup_phase_error) {
		LOG_INFO("cancel-cleanup-can-log");
		CHECK(logger_flush_status() == 0);
	}
	atomic_store(&cleanup_seen, 1);
	/* Thread is exiting, deliberately no re-enable in application cleanup. */
}
static void *caller(void *unused)
{
	(void)unused;
	int async_case = !strcmp(scenario, "async-cancel-write");
	int saved_state = PTHREAD_CANCEL_ENABLE;
	int saved_type = PTHREAD_CANCEL_DEFERRED;
	/*
	 * POSIX only permits cancellation cleanup scopes to be manipulated while
	 * cancellation is deferred or disabled. Install the handler while disabled,
	 * then restore the caller's enabled state before entering Logger. The test
	 * sends pthread_cancel() only after Logger's own scope is observed disabled.
	 */
	if (async_case) {
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					      &saved_state));
		CHECK(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					     &saved_type));
	}
	pthread_cleanup_push(cleanup, NULL);
	if (async_case)
		CHECK(!pthread_setcancelstate(saved_state, NULL));
	if (!strcmp(scenario, "create") || !strcmp(scenario, "create-fail")) {
		logger_config_t cfg = config_for("old.log", 0);
		int rc = logger_init(&cfg);
		CHECK(rc == (!strcmp(scenario, "create-fail") ? -1 : 0));
	} else if (!strcmp(scenario, "shutdown")) {
		CHECK(stop_status() == 0);
	} else if (!strcmp(scenario, "control-wait")) {
		delay_controller = 1;
		logger_config_t cfg = config_for("side.log", 0);
		CHECK(logger_init(&cfg) == 0);
	} else if (!strcmp(scenario, "reopen")) {
		CHECK(logger_reopen() == 0);
	} else if (!strcmp(scenario, "flush") ||
		   !strcmp(scenario, "flush-void")) {
		watch_cond = 1;
		if (!strcmp(scenario, "flush"))
			CHECK(logger_flush_status() == 0);
		else
			logger_flush();
	} else if (!strcmp(scenario, "reader")) {
		delay_reader = 1;
		CHECK(logger_flush_status() == 0);
	} else {
		LOG_INFO("cancelled-operation-record");
	}
	atomic_store(&operation_returned, 1);
	/* Deferred cancellation needs an explicit point; an async pending request
	 * may already have been acted on when Logger restored the saved state. */
	pthread_testcancel();
	if (async_case) {
		int current_state;
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					      &current_state));
		CHECK(!pthread_setcanceltype(saved_type, NULL));
	}
	pthread_cleanup_pop(0);
	if (async_case)
		CHECK(!pthread_setcancelstate(saved_state, NULL));
	return NULL;
}
static void restore_policy(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	CHECK(!logger_init(&cfg));
	LOG_INFO("caller-disabled");
	CHECK(!logger_flush_status());
	CHECK(!stop_status());
	int previous;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
	CHECK(previous == PTHREAD_CANCEL_DISABLE);
	CHECK(!pthread_setcancelstate(old, NULL));
	CHECK(!logger_init(&cfg));
	LOG_INFO("caller-enabled");
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
	CHECK(previous == PTHREAD_CANCEL_ENABLE);
	CHECK(!pthread_setcancelstate(previous, NULL));
	CHECK(!stop_status());
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	scenario = argv[1];
	fixture_reset();
	if (!strcmp(scenario, "restore-policy")) {
		restore_policy();
		return 0;
	}
	int no_initial = !strcmp(scenario, "create") ||
			 !strcmp(scenario, "create-fail") ||
			 !strcmp(scenario, "bootstrap") ||
			 !strcmp(scenario, "control-wait");
	int async = !strcmp(scenario, "queue") || !strcmp(scenario, "flush") ||
		    !strcmp(scenario, "flush-void");
	logger_config_t cfg = config_for("old.log", async);
	if (!no_initial)
		CHECK(!logger_init(&cfg));
	cleanup_phase_error = !strcmp(scenario, "shutdown") ? ESHUTDOWN :
			      (!strcmp(scenario, "bootstrap") ||
			       !strcmp(scenario, "create-fail")) ?
							      ENODEV :
							      0;
	if (!strcmp(scenario, "create") || !strcmp(scenario, "create-fail")) {
		atomic_store(&stage, P_CREATE);
		atomic_store(&create_fails, !strcmp(scenario, "create-fail"));
	} else if (!strcmp(scenario, "shutdown"))
		atomic_store(&stage, P_DISPOSE);
	else if (!strcmp(scenario, "reopen"))
		atomic_store(&stage, P_REOPEN);
	else if (!strcmp(scenario, "bootstrap"))
		atomic_store(&stage, P_BOOTSTRAP);
	else if (!strcmp(scenario, "queue"))
		atomic_store(&stage, P_QUEUE);
	else if (!strcmp(scenario, "reader"))
		atomic_store(&stage, P_READER);
	else if (!strcmp(scenario, "control-wait"))
		atomic_store(&stage, P_CONTROL);
	else if (!strcmp(scenario, "flush") ||
		 !strcmp(scenario, "flush-void")) {
		atomic_store(&hold_format, 1);
		LOG_INFO("worker-must-complete-before-cancel");
		wait_flag(&format_entered);
	} else {
		CHECK(!strcmp(scenario, "write") ||
		      !strcmp(scenario, "async-cancel-write"));
		atomic_store(&stage, P_WRITE);
	}
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, caller, NULL));
	if (!strcmp(scenario, "flush") || !strcmp(scenario, "flush-void"))
		wait_flag(&flush_waiting);
	else
		wait_flag(&at_point);
	CHECK(atomic_load(&seen_disabled));
	CHECK(!pthread_cancel(thread));
	for (int i = 0; i < 20; ++i)
		nap_ms();
	CHECK(!atomic_load(
		&cleanup_seen)); /* cancellation must not strand held resources */
	atomic_store(&release_point, 1);
	atomic_store(&release_format, 1);
	void *ret;
	CHECK(!pthread_join(thread, &ret));
	CHECK(ret == PTHREAD_CANCELED && atomic_load(&cleanup_seen));
	if (!cleanup_phase_error) {
		LOG_INFO("main-after-cancel");
		CHECK(!logger_flush_status());
		CHECK(!stop_status());
		const char *path = !strcmp(scenario, "control-wait") ?
					   "side.log" :
					   "old.log";
		CHECK(file_contains(path, "cancel-cleanup-can-log"));
		CHECK(file_contains(path, "main-after-cancel"));
		if (!strcmp(scenario, "write") || !strcmp(scenario, "queue") ||
		    !strcmp(scenario, "async-cancel-write"))
			CHECK(file_contains(path,
					    "cancelled-operation-record"));
	} else
		CHECK(!stop_status());
	CHECK(!logger_process_object_count());
	/* No abandoned global control/read/instance lock; another generation works. */
	CHECK(!logger_init(&cfg));
	LOG_INFO("next-generation-after-cancel");
	CHECK(!logger_flush_status() && !stop_status());
	puts("global cancellation deferral and post-cancel reuse passed");
	return 0;
}
