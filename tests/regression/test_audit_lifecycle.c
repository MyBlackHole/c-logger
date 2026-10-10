#define _GNU_SOURCE
#include "audit_support.h"
#include "logger_internal.h"
#include <fcntl.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/wait.h>

static _Atomic int pause_start, pause_stop, pause_destroy, pause_write;
static _Atomic int entered, release_point, init_attempted, delayed_entered,
	release_delayed;
static _Thread_local int delay_operation;
static _Thread_local int mark_initializer;
static _Atomic int initializer_at_lock;
static _Thread_local logger_t *destroy_subject;
/* One-shot link wrappers: no production test hooks or new Audit ABI. */
static _Atomic int fail_progress_cond, fail_progress_mutex;
static _Atomic int fail_start_record, fail_checkpoint_close;
static _Atomic int fail_without_destroy, destroy_calls;

static void check_writer_owner(const char *expect)
{
	int fd = open("app.audit.log.logger.lock", O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	int rc = flock(fd, LOCK_EX | LOCK_NB);
	int error = errno;
	if (!strcmp(expect, "busy"))
		CHECK(rc == -1 && (error == EAGAIN || error == EWOULDBLOCK));
	else
		CHECK(rc == 0);
	CHECK(close(fd) == 0);
}

int __real_logger_destroy_status(logger_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_cond_destroy(pthread_cond_t *);
int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __real_logger_file_close_status(logger_file_t *);
int __real_logger_log_sync_status(logger_t *, logger_level_t, const char *,
				  const char *, int, const char *, const char *,
				  ...);

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (mark_initializer) {
		mark_initializer = 0;
		atomic_store(&initializer_at_lock, 1);
	}
	if (delay_operation) {
		delay_operation = 0;
		atomic_store(&delayed_entered, 1);
		wait_flag(&release_delayed);
	}
	return __real_pthread_mutex_lock(mu);
}
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (destroy_subject && cv == &destroy_subject->progress_cv &&
	    atomic_exchange(&fail_progress_cond, 0))
		return EBUSY;
	return __real_pthread_cond_destroy(cv);
}
int __wrap_pthread_mutex_destroy(pthread_mutex_t *mu)
{
	if (destroy_subject && mu == &destroy_subject->progress_mu &&
	    atomic_exchange(&fail_progress_mutex, 0))
		return EBUSY;
	return __real_pthread_mutex_destroy(mu);
}
int __wrap_logger_file_close_status(logger_file_t *f)
{
	/* Only the Audit-owned checkpoint lease, after it has actually been
	 * closed. This is a finalization I/O error, NOT a lifetime failure. */
	int checkpoint = strstr(f->path, ".audit.state") != NULL;
	int rc = __real_logger_file_close_status(f);
	if (checkpoint && atomic_exchange(&fail_checkpoint_close, 0))
		return -EIO;
	return rc;
}
int __wrap_logger_destroy_status(logger_t *l)
{
	if (l && atomic_exchange(&pause_destroy, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_point);
	}
	if (l)
		atomic_fetch_add(&destroy_calls, 1);
	if (l && atomic_exchange(&fail_without_destroy, 0)) {
		/* Simulate a failed finalization before the inner destructor:
		 * its final-release receipt must remain false. */
		errno = EDEADLK;
		return -1;
	}
	destroy_subject = l;
	int rc = __real_logger_destroy_status(l);
	destroy_subject = NULL;
	return rc;
}
int __wrap_logger_log_sync_status(logger_t *l, logger_level_t level,
				  const char *module, const char *file,
				  int line, const char *func, const char *fmt,
				  ...)
{
	char text[16384];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	CHECK(n >= 0 && (size_t)n < sizeof(text));
	int pause = 0;
	if (strstr(text, "event=\"AUDIT_START\"")) {
		pause = atomic_exchange(&pause_start, 0);
		if (atomic_exchange(&fail_start_record, 0))
			return -EIO; /* create_runtime fails before publication */
	}
	if (strstr(text, "event=\"AUDIT_STOP\""))
		pause = atomic_exchange(&pause_stop, 0);
	if (strstr(text, "event=\"LATE\""))
		pause = atomic_exchange(&pause_write, 0);
	if (pause) {
		atomic_store(&entered, 1);
		wait_flag(&release_point);
	}
	return __real_logger_log_sync_status(l, level, module, file, line, func,
					     "%s", text);
}

typedef struct {
	int rc;
	int error;
	_Atomic int done;
} answer_t;
static void *initializer(void *ptr)
{
	answer_t *a = ptr;
	audit_config_t c = audit_test_config();
	atomic_store(&init_attempted, 1);
	mark_initializer = 1;
	a->rc = audit_init(&c);
	a->error = errno;
	atomic_store(&a->done, 1);
	return NULL;
}

static void *report_initializer(void *ptr)
{
	answer_t *a = ptr;
	audit_config_t c = audit_test_config();
	c.failure_policy = AUDIT_FAIL_REPORT;
	atomic_store(&init_attempted, 1);
	mark_initializer = 1;
	a->rc = audit_init(&c);
	a->error = errno;
	atomic_store(&a->done, 1);
	return NULL;
}
static void *stopper(void *ptr)
{
	answer_t *a = ptr;
#ifndef AUDIT_BASELINE_PROBE
	a->rc = audit_shutdown_status();
#else
	audit_shutdown();
	a->rc = 0;
#endif
	a->error = errno;
	atomic_store(&a->done, 1);
	return NULL;
}
static void *writer(void *ptr)
{
	answer_t *a = ptr;
	audit_event_t e = audit_test_event("LATE");
	a->rc = audit_write(&e);
	a->error = errno;
	atomic_store(&a->done, 1);
	return NULL;
}

static void init_gate(void)
{
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	atomic_store(&pause_start, 1);
	pthread_t thread;
	answer_t a = { 0 };
	CHECK(!pthread_create(&thread, NULL, report_initializer, &a));
	wait_flag(&entered);
	/* Candidate exists, but STARTING is not a published Audit session. */
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	char id[33];
	int copy_rc = audit_instance_id_copy(id);
	int copy_error = errno;
	atomic_store(&release_point, 1);
	CHECK(!pthread_join(thread, NULL));
	CHECK(a.rc == 0);
	CHECK(audit_failure_policy() == AUDIT_FAIL_REPORT);
	audit_shutdown();
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	CHECK(copy_rc == -1 && copy_error == EAGAIN);
	CHECK(count_event("AUDIT_START") == 1 &&
	      count_event("AUDIT_STOP") == 1);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void stop_gate(void)
{
	audit_config_t c = audit_test_config();
	c.failure_policy = AUDIT_FAIL_REPORT;
	CHECK(audit_init(&c) == 0);
	CHECK(audit_failure_policy() == AUDIT_FAIL_REPORT);
	atomic_store(&pause_stop, 1);
	pthread_t shutdown_thread, write_thread;
	answer_t stop = { 0 }, write = { 0 };
	CHECK(!pthread_create(&shutdown_thread, NULL, stopper, &stop));
	wait_flag(&entered);
	/* STOPPING closes policy admission before durable teardown completes. */
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	CHECK(!pthread_create(&write_thread, NULL, writer, &write));
	/* Bounded wait ensures a regression never leaves the runner hung. */
	for (int i = 0; i < 100 && !atomic_load(&write.done); ++i)
		nap_ms();
	int rejected_before_release = atomic_load(&write.done);
	atomic_store(&release_point, 1);
	CHECK(!pthread_join(shutdown_thread, NULL));
	CHECK(!pthread_join(write_thread, NULL));
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	CHECK(rejected_before_release && write.rc == -1 &&
	      write.error == ESHUTDOWN);
	CHECK(stop.rc == 0 && !count_event("LATE"));
}

static void teardown_race(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	atomic_store(&pause_destroy, 1);
	pthread_t old, fresh;
	answer_t stop = { 0 }, init = { 0 };
	CHECK(!pthread_create(&old, NULL, stopper, &stop));
	wait_flag(&entered);
	CHECK(!pthread_create(&fresh, NULL, initializer, &init));
	wait_flag(&initializer_at_lock);
	check_writer_owner("busy");
	for (int i = 0; i < 100 && !atomic_load(&init.done); ++i)
		nap_ms();
	int too_early = atomic_load(&init.done);
	atomic_store(&release_point, 1);
	CHECK(!pthread_join(old, NULL));
	CHECK(!pthread_join(fresh, NULL));
	CHECK(stop.rc == 0 && init.rc == 0);
	/* Old shutdown must never release the new session's writer ownership. */
	check_writer_owner("busy");
	audit_shutdown();
	CHECK(!too_early);
	check_writer_owner("free");
	CHECK(audit_verify_file("app.audit.log") == 0);
}

#ifndef AUDIT_BASELINE_PROBE
static void expect_retired(int error, unsigned destroy_count)
{
	audit_status_t st;
	CHECK(audit_get_status(&st) == 0);
	CHECK(st.state == AUDIT_STATE_STOPPING && st.error_code == error);
	CHECK(audit_failure_policy() == AUDIT_FAIL_DENY);
	CHECK(logger_process_object_count() == 1);
	check_writer_owner("busy");

	/* Closing a failed private logger was intentionally incomplete.
	 * No shutdown retry or new candidate may touch those resources. */
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == -1 && errno == error);
	CHECK(audit_shutdown_status() == -1 && errno == error);
	CHECK(atomic_load(&destroy_calls) == (int)destroy_count);
	CHECK(logger_process_object_count() == 1);
	check_writer_owner("busy");
}

static void failed_disposal(const char *mode)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	if (!strcmp(mode, "cond"))
		atomic_store(&fail_progress_cond, 1);
	else if (!strcmp(mode, "mutex"))
		atomic_store(&fail_progress_mutex, 1);
	else if (!strcmp(mode, "no-final-release"))
		atomic_store(&fail_without_destroy, 1);
	else
		CHECK(!"bad disposal mode");
	int expected = !strcmp(mode, "no-final-release") ? EDEADLK : EBUSY;
	CHECK(audit_shutdown_status() == -1 && errno == expected);
	expect_retired(expected, 1);
	CHECK(count_event("AUDIT_START") == 1);
	CHECK(count_event("AUDIT_STOP") == 1);
}

static void failed_init_rollback(void)
{
	audit_config_t c = audit_test_config();
	atomic_store(&fail_start_record, 1);
	atomic_store(&fail_progress_cond, 1);
	CHECK(audit_init(&c) == -1 && errno == EIO);
	/* The original START failure is reported for this attempt, but the
	 * failed candidate destructor's EBUSY permanently blocks recovery. */
	expect_retired(EBUSY, 1);
	CHECK(!count_event("AUDIT_START") && !count_event("AUDIT_STOP"));
}

static void close_error_final_release(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	atomic_store(&fail_checkpoint_close, 1);
	CHECK(audit_shutdown_status() == -1 && errno == EIO);
	CHECK(logger_process_object_count() == 0);
	check_writer_owner("free");

	audit_status_t st;
	CHECK(audit_get_status(&st) == 0);
	CHECK(st.state == AUDIT_STATE_IDLE && st.error_code == EIO);
	CHECK(audit_init(&c) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(logger_process_object_count() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void teardown_failure_race(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	atomic_store(&pause_destroy, 1);
	atomic_store(&fail_progress_cond, 1);

	pthread_t old, fresh;
	answer_t stop = { 0 }, init = { 0 };
	CHECK(!pthread_create(&old, NULL, stopper, &stop));
	wait_flag(&entered); /* old controller owns retired candidate soon */
	CHECK(!pthread_create(&fresh, NULL, initializer, &init));
	wait_flag(&initializer_at_lock); /* new init is queued on control */
	check_writer_owner("busy");
	CHECK(!atomic_load(&init.done));
	atomic_store(&release_point, 1);
	CHECK(!pthread_join(old, NULL) && !pthread_join(fresh, NULL));
	CHECK(stop.rc == -1 && stop.error == EBUSY);
	CHECK(init.rc == -1 && init.error == EBUSY);
	expect_retired(EBUSY, 1);
	CHECK(count_event("AUDIT_START") == 1);
}

static void *cancelled_writer(void *ptr)
{
	(void)writer(ptr);
	pthread_testcancel();
	return NULL;
}
static void cancel_operation(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	atomic_store(&pause_write, 1);
	answer_t a = { 0 };
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, cancelled_writer, &a));
	wait_flag(&entered);
	CHECK(!pthread_cancel(thread));
	atomic_store(&release_point, 1);
	void *ret;
	CHECK(!pthread_join(thread, &ret) && ret == PTHREAD_CANCELED);
	audit_event_t e = audit_test_event("AFTER_CANCEL");
	CHECK(audit_write(&e) ==
	      0); /* operation/control locks must remain usable */
	CHECK(audit_shutdown_status() == 0);
	CHECK(count_event("LATE") == 1 && count_event("AFTER_CANCEL") == 1);
	CHECK(audit_verify_file("app.audit.log") == 0);
}
static void *delayed_writer(void *ptr)
{
	delay_operation = 1;
	return writer(ptr);
}
static void delayed_admission(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	pthread_t thread;
	answer_t a = { 0 };
	CHECK(!pthread_create(&thread, NULL, delayed_writer, &a));
	wait_flag(
		&delayed_entered); /* after phase snapshot, before the operation lock */
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_init(&c) == 0);
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(thread, NULL));
	CHECK(a.rc == -1 && a.error == ESHUTDOWN);
	CHECK(!count_event("LATE"));
	CHECK(audit_shutdown_status() == 0);
}

static void guard(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
				    .event = "NO",
				    .operation = "probe" };
		char id[33];
		if (audit_init(&c) != -1 || errno != ECHILD)
			_exit(10);
		if (audit_write(&e) != -1 || errno != ECHILD)
			_exit(11);
		if (audit_flush() != -1 || errno != ECHILD)
			_exit(12);
		if (audit_instance_id_copy(id) != -1 || errno != ECHILD)
			_exit(13);
		if (audit_shutdown_status() != -1 || errno != ECHILD)
			_exit(14);
		if (audit_failure_policy() != AUDIT_FAIL_DENY)
			_exit(15);
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	check_writer_owner("busy");
	CHECK(audit_shutdown_status() == 0);
}
#endif

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-lifecycle-XXXXXX";
	enter_temp(dir);
	if (!strcmp(argv[1], "init-gate"))
		init_gate();
	else if (!strcmp(argv[1], "stop-gate"))
		stop_gate();
	else if (!strcmp(argv[1], "teardown-race"))
		teardown_race();
#ifndef AUDIT_BASELINE_PROBE
	else if (!strcmp(argv[1], "dispose-cond"))
		failed_disposal("cond");
	else if (!strcmp(argv[1], "dispose-mutex"))
		failed_disposal("mutex");
	else if (!strcmp(argv[1], "dispose-no-final-release"))
		failed_disposal("no-final-release");
	else if (!strcmp(argv[1], "init-rollback-retain"))
		failed_init_rollback();
	else if (!strcmp(argv[1], "close-error-finalized"))
		close_error_final_release();
	else if (!strcmp(argv[1], "teardown-failure-race"))
		teardown_failure_race();
	else if (!strcmp(argv[1], "generation"))
		delayed_admission();
	else if (!strcmp(argv[1], "cancel"))
		cancel_operation();
	else if (!strcmp(argv[1], "fork-guard"))
		guard();
#endif
	else
		CHECK(!"unknown mode");
	audit_test_cleanup(dir);
	return 0;
}
