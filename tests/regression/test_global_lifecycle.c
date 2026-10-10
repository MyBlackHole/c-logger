#define _GNU_SOURCE
#include "global_support.h"
#include <stdarg.h>
#include <sys/wait.h>

/* No hooks in the library. GNU ld wrappers control exact before-lock and
 * in-backend boundaries; the executable owns every pause and failure. */
static _Thread_local int delay_read, delay_control, count_reads, note_writer;
static _Thread_local int fail_read, fail_control, fail_writer;
static _Atomic int delayed, release_delayed, entered, release_backend;
static _Atomic int writer_waiting, read_count, pause_create, pause_destroy,
	pause_write;
static _Atomic int fail_create, create_count, fail_dispose;
static _Atomic int recurse_create, recurse_destroy, recurse_write,
	recurse_reopen, recurse_bootstrap;
static _Atomic int recursion_checked, pause_bootstrap, fail_sync;
static int admission_only;
/* Link-time failures do not call the real release: lock ownership remains
 * deliberately uncertain. Every poison scenario runs in its own process. */
static _Thread_local int fail_rw_unlock, fail_control_unlock;
static _Thread_local int fail_join, fail_cond_destroy;
static _Thread_local int handoff_reader, handoff_writer;
static _Atomic int handoff_reader_at_unlock, handoff_writer_at_lock;
static _Atomic int handoff_release_reader, handoff_release_writer;
static _Atomic int handoff_fail_writer;
static _Thread_local int capture_control;
static _Thread_local pthread_t rejected_worker;
static _Thread_local int saw_rejected_join;
static pthread_mutex_t *control_mutex;
int __real_fsync(int);
int __wrap_fsync(int fd)
{
	if (atomic_exchange(&fail_sync, 0)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *);
int __real_pthread_join(pthread_t, void **);
int __real_pthread_cond_destroy(pthread_cond_t *);
logger_t *__real_logger_create(const logger_config_t *);
int __real_logger_destroy_status(logger_t *);
int __real_logger_file_write(logger_file_t *, const char *, size_t,
			     const struct timespec *, int);
int __real_logger_file_reopen(logger_file_t *);

static void verify_reentry(void)
{
	logger_config_t cfg = config_for("side.log", 0);
	CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
	CHECK(stop_status() == -1 && errno == EDEADLK);
	for (int i = 0; i < OP_COUNT; ++i) {
		call_result_t r = { .op = (enum operation)i };
		invoke(&r);
		expect_rejection(&r, EDEADLK);
	}
	CHECK(access("side.log", F_OK) == -1);
	atomic_fetch_add(&recursion_checked, 1);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (capture_control) {
		control_mutex = mu;
		capture_control = 0;
	}
	if (delay_control) {
		delay_control = 0;
		atomic_store(&delayed, 1);
		wait_flag(&release_delayed);
	}
	if (fail_control) {
		int rc = fail_control;
		fail_control = 0;
		return rc;
	}
	return __real_pthread_mutex_lock(mu);
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (mu == control_mutex && fail_control_unlock) {
		int error = fail_control_unlock;
		fail_control_unlock = 0;
		return error; /* owner is still holding the real mutex */
	}
	return __real_pthread_mutex_unlock(mu);
}
int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
	if (handoff_reader) {
		handoff_reader = 0;
		/* Synthetic error after a real release: a caller seeing a
		 * nonzero return must still treat ownership as unknown, while
		 * the other real pthread thread can safely acquire the lock. */
		atomic_store(&handoff_reader_at_unlock, 1);
		wait_flag(&handoff_release_reader);
		CHECK(__real_pthread_rwlock_unlock(rw) == 0);
		return EIO;
	}
	if (fail_rw_unlock) {
		int error = fail_rw_unlock;
		fail_rw_unlock = 0;
		return error; /* no actual unlock: ownership is uncertain */
	}
	return __real_pthread_rwlock_unlock(rw);
}
int __wrap_pthread_join(pthread_t thread, void **result)
{
	if (fail_join) {
		int error = fail_join;
		fail_join = 0;
		rejected_worker = thread;
		saw_rejected_join = 1;
		return error; /* production join failed: final free is forbidden */
	}
	return __real_pthread_join(thread, result);
}
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (fail_cond_destroy) {
		int error = fail_cond_destroy;
		fail_cond_destroy = 0;
		return error;
	}
	return __real_pthread_cond_destroy(cv);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (count_reads)
		atomic_fetch_add(&read_count, 1);
	if (delay_read) {
		delay_read = 0;
		atomic_store(&delayed, 1);
		wait_flag(&release_delayed);
	}
	if (fail_read) {
		int rc = fail_read;
		fail_read = 0;
		return rc;
	}
	return __real_pthread_rwlock_rdlock(rw);
}
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	if (handoff_writer) {
		handoff_writer = 0;
		atomic_store(&handoff_writer_at_lock, 1);
		wait_flag(&handoff_release_writer);
	}
	if (note_writer) {
		note_writer = 0;
		atomic_store(&writer_waiting, 1);
	}
	if (fail_writer) {
		int rc = fail_writer;
		fail_writer = 0;
		return rc;
	}
	return __real_pthread_rwlock_wrlock(rw);
}
logger_t *__wrap_logger_create(const logger_config_t *cfg)
{
	atomic_fetch_add(&create_count, 1);
	if (atomic_exchange(&pause_create, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_create, 0))
		verify_reentry();
	if (atomic_exchange(&fail_create, 0)) {
		errno = ENOSPC;
		return NULL;
	}
	return __real_logger_create(cfg);
}
int __wrap_logger_destroy_status(logger_t *l)
{
	if (atomic_exchange(&pause_destroy, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_destroy, 0))
		verify_reentry();
	int rc = __real_logger_destroy_status(l);
	if (atomic_exchange(&fail_dispose, 0)) {
		errno = EIO;
		return -1;
	}
	return rc;
}
int __wrap_logger_file_write(logger_file_t *f, const char *text, size_t n,
			     const struct timespec *ts, int sync)
{
	if (atomic_exchange(&pause_write, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_write, 0))
		verify_reentry();
	return __real_logger_file_write(f, text, n, ts, sync);
}
int __wrap_logger_file_reopen(logger_file_t *f)
{
	if (atomic_exchange(&recurse_reopen, 0))
		verify_reentry();
	return __real_logger_file_reopen(f);
}
int __wrap_dprintf(int fd, const char *fmt, ...)
{
	if (atomic_exchange(&pause_bootstrap, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_bootstrap, 0))
		verify_reentry();
	va_list ap;
	va_start(ap, fmt);
	int rc = vdprintf(fd, fmt, ap);
	va_end(ap);
	return rc;
}

static void *delayed_call(void *p)
{
	delay_read = 1;
	invoke(p);
	return NULL;
}
static void *shutdown_call(void *p)
{
	call_result_t *r = p;
	note_writer = 1;
	r->rc = stop_status();
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}
static void *delayed_shutdown(void *p)
{
	delay_control = 1;
	return shutdown_call(p);
}
static void *init_call(void *p)
{
	call_result_t *r = p;
	logger_config_t cfg = config_for("old.log", 0);
	r->rc = logger_init(&cfg);
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}
static void *write_call(void *p)
{
	call_result_t *r = p;
	LOG_INFO("pinned-before-stop");
	atomic_store(&r->done, 1);
	return NULL;
}

static void generation(enum operation op, int bootstrap)
{
	logger_config_t a = config_for("old.log", 0),
			b = config_for("new.log", 0);
	if (!bootstrap) {
		CHECK(logger_init(&a) == 0);
		LOG_INFO("old-base");
	}
	pthread_t t;
	call_result_t r = { .op = op };
	CHECK(!pthread_create(&t, NULL, delayed_call, &r));
	wait_flag(&delayed); /* ticket sampled, before reader lock */
	if (!bootstrap)
		CHECK(stop_status() == 0);
	CHECK(logger_init(&b) == 0);
	LOG_INFO("new-base");
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(t, NULL));
	expect_rejection(&r, ESHUTDOWN);
	LOG_INFO("new-after-stale");
	CHECK(logger_flush_status() == 0);
	CHECK(stop_status() == 0);
	CHECK(file_lines("new.log") == 2 &&
	      file_contains("new.log", "new-after-stale"));
	CHECK(!file_contains("new.log", "delayed-record"));
	if (!bootstrap)
		CHECK(file_lines("old.log") == 1);
}
static void shutdown_generation(void)
{
	logger_config_t a = config_for("old.log", 0),
			b = config_for("new.log", 0);
	CHECK(!logger_init(&a));
	pthread_t t;
	call_result_t r = { 0 };
	CHECK(!pthread_create(&t, NULL, delayed_shutdown, &r));
	wait_flag(
		&delayed); /* shutdown sampled old generation, no control lock */
	CHECK(!stop_status());
	CHECK(!logger_init(&b));
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(t, NULL));
	CHECK(r.rc == -1 && r.error == ESHUTDOWN);
	LOG_INFO("new-not-stopped");
	CHECK(!logger_flush_status() && !stop_status());
	CHECK(file_contains("new.log", "new-not-stopped"));
}
static void all_rejected(int error)
{
	for (int i = 0; i < OP_COUNT; ++i) {
		call_result_t r = { .op = (enum operation)i };
		/* Verify rejected metric queries overwrite old caller data with zero. */
		memset(&r.metrics, 0xa5, sizeof(r.metrics));
		memset(&r.io, 0xa5, sizeof(r.io));
		memset(&r.diagnostics, 0xa5, sizeof(r.diagnostics));
		memset(&r.file_metrics, 0xa5, sizeof(r.file_metrics));
		invoke(&r);
		expect_rejection(&r, error);
		if (r.op == OP_FILE_METRICS)
			CHECK(((const unsigned char *)&r.file_metrics)[0] == 0xa5);
	}
}
static void start_gate(void)
{
	pthread_t t;
	call_result_t r = { 0 };
	atomic_store(&pause_create, 1);
	CHECK(!pthread_create(&t, NULL, init_call, &r));
	wait_flag(&entered); /* before candidate exists */
	count_reads = 1;
	all_rejected(EAGAIN);
	count_reads = 0;
	CHECK(!atomic_load(&read_count));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(t, NULL) && r.rc == 0);
	LOG_INFO("published-only-now");
	CHECK(!stop_status() && file_lines("old.log") == 1);
}
static void *flood_closed(void *p)
{
	(void)p;
	count_reads = 1;
	for (int i = 0; i < 100; ++i) {
		if (admission_only) {
			logger_metrics_t metrics;
			logger_get_global_metrics(&metrics);
			logger_diagnostics_t diagnostics;
			logger_get_global_diagnostics(&diagnostics);
			(void)logger_global_dropped();
			logger_set_level(LOGGER_INFO);
		} else
			all_rejected(ESHUTDOWN);
	}
	return NULL;
}
static void stop_gate(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pthread_t writer, stopper, readers[8];
	call_result_t w = { 0 }, s = { 0 };
	atomic_store(&pause_write, 1);
	CHECK(!pthread_create(&writer, NULL, write_call, &w));
	wait_flag(&entered); /* existing reader is inside backend emit_mu */
	CHECK(!pthread_create(&stopper, NULL, shutdown_call, &s));
	wait_flag(&writer_waiting); /* admission closed, wrlock about to wait */
	for (int i = 0; i < 8; ++i)
		CHECK(!pthread_create(&readers[i], NULL, flood_closed, NULL));
	for (int i = 0; i < 8; ++i)
		CHECK(!pthread_join(readers[i], NULL));
	CHECK(!atomic_load(&read_count));
	CHECK(!atomic_load(&s.done));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(writer, NULL) && !pthread_join(stopper, NULL));
	CHECK(!s.rc && file_lines("old.log") == 1);
	CHECK(file_contains("old.log", "pinned-before-stop"));
}
static void bootstrap_stop(void)
{
	pthread_t w, s;
	call_result_t wr = { 0 }, sr = { 0 };
	atomic_store(&pause_bootstrap, 1);
	CHECK(!pthread_create(&w, NULL, write_call, &wr));
	wait_flag(&entered);
	CHECK(!pthread_create(&s, NULL, shutdown_call, &sr));
	wait_flag(&writer_waiting);
	for (int i = 0; i < 20; ++i)
		nap_ms();
	CHECK(!atomic_load(&sr.done));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(w, NULL) && !pthread_join(s, NULL));
	CHECK(!sr.rc);
	all_rejected(ESHUTDOWN);
}
static void during_start_stop(void)
{
	pthread_t init, stop;
	call_result_t i = { 0 }, s = { 0 };
	atomic_store(&pause_create, 1);
	CHECK(!pthread_create(&init, NULL, init_call, &i));
	wait_flag(&entered);
	CHECK(!pthread_create(&stop, NULL, delayed_shutdown, &s));
	wait_flag(&delayed);
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(init, NULL) && !i.rc);
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(stop, NULL) && !s.rc);
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
	CHECK(logger_process_object_count() == 0);
}
static void double_shutdown(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pthread_t one, two;
	call_result_t a = { 0 }, b = { 0 };
	atomic_store(&pause_destroy, 1);
	CHECK(!pthread_create(&one, NULL, shutdown_call, &a));
	wait_flag(&entered);
	CHECK(!pthread_create(&two, NULL, delayed_shutdown, &b));
	wait_flag(&delayed);
	atomic_store(&release_delayed, 1);
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(one, NULL) && !pthread_join(two, NULL));
	CHECK(!a.rc && !b.rc && !logger_process_object_count());
}
static void recursion(const char *origin)
{
	logger_config_t cfg = config_for("old.log", 0);
	if (!strcmp(origin, "bootstrap")) {
		atomic_store(&recurse_bootstrap, 1);
		LOG_INFO("bootstrap");
		CHECK(atomic_load(&recursion_checked) == 1);
		return;
	}
	if (!strcmp(origin, "create"))
		atomic_store(&recurse_create, 1);
	CHECK(!logger_init(&cfg));
	if (!strcmp(origin, "write")) {
		atomic_store(&recurse_write, 1);
		LOG_INFO("outer-survived");
	} else if (!strcmp(origin, "reopen")) {
		atomic_store(&recurse_reopen, 1);
		CHECK(!logger_reopen());
	} else if (!strcmp(origin, "destroy"))
		atomic_store(&recurse_destroy, 1);
	CHECK(!stop_status());
	CHECK(atomic_load(&recursion_checked) == 1);
	CHECK(!logger_process_object_count());
}
static void acquisition_error(const char *which)
{
	logger_config_t cfg = config_for("old.log", 0);
	if (!strcmp(which, "control")) {
		fail_control = EBUSY;
		CHECK(logger_init(&cfg) == -1 && errno == EBUSY);
		CHECK(!atomic_load(&create_count));
	} else if (!strcmp(which, "publish")) {
		fail_writer = EDEADLK;
		CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
		CHECK(!logger_process_object_count());
	} else if (!strcmp(which, "create")) {
		atomic_store(&fail_create, 1);
		CHECK(logger_init(&cfg) == -1 && errno == ENOSPC);
		CHECK(!logger_process_object_count());
		CHECK(logger_flush_status() == -1 && errno == ENODEV);
	}
	CHECK(!logger_init(&cfg));
	if (!strcmp(which, "read")) {
		fail_read = EAGAIN;
		CHECK(logger_flush_status() == -1 && errno == EAGAIN);
	} else if (!strcmp(which, "stop")) {
		fail_writer = EDEADLK;
		CHECK(stop_status() == -1 && errno == EDEADLK);
	}
	LOG_INFO("after-error");
	CHECK(!logger_flush_status() && !stop_status());
	CHECK(file_lines("old.log") == 1 && !logger_process_object_count());
}
static void *verify_poisoned_controller(void *arg)
{
	int error = *(int *)arg;
	all_rejected(error);
	logger_config_t cfg = config_for("new.log", 0);
	CHECK(logger_init(&cfg) == -1 && errno == error);
	CHECK(stop_status() == -1 && errno == error);
	return NULL;
}

/* Reader has acquired the lifetime pin before reaching release.
 * The controller closes admission, pauses before acquiring writer, then
 * the reader returns a synthetic unlock error. Both interleavings must
 * preserve BROKEN: writer success must not unpublish, and failed writer
 * acquisition must not roll back the already-poisoned generation. */
static void *handoff_reader_thread(void *arg)
{
	call_result_t *r = arg;
	handoff_reader = 1;
	r->rc = logger_flush_status();
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}

static void *handoff_shutdown_thread(void *arg)
{
	handoff_writer = 1;
	if (atomic_load(&handoff_fail_writer))
		fail_writer = EDEADLK;
	return shutdown_call(arg);
}

static void unlock_shutdown_handoff(int acquire_fails)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(logger_init(&cfg) == 0);
	atomic_store(&handoff_fail_writer, acquire_fails);

	pthread_t reader, controller;
	call_result_t a = { 0 }, b = { 0 };
	CHECK(!pthread_create(&reader, NULL, handoff_reader_thread, &a));
	wait_flag(&handoff_reader_at_unlock);
	/* Reader still owns the lock; shutdown must close admission first. */
	CHECK(!pthread_create(&controller, NULL, handoff_shutdown_thread,
				 &b));
	wait_flag(&handoff_writer_at_lock);

	/* For this controlled error, the real unlock succeeds. The injected
	 * error is returned only after the reader has released that pin. */
	atomic_store(&handoff_release_reader, 1);
	CHECK(!pthread_join(reader, NULL));
	CHECK(a.rc == -1 && a.error == EIO);

	atomic_store(&handoff_release_writer, 1);
	CHECK(!pthread_join(controller, NULL));
	if (acquire_fails)
		CHECK(b.rc == -1 && b.error == EDEADLK);
	else
		CHECK(b.rc == -1 && b.error == EIO);

	/* The reader's poison wins even if writer acquires successfully.
	 * The one live instance is still owned by the Global controller. */
	CHECK(logger_process_object_count() == 1);
	all_rejected(EIO);
	logger_config_t replacement = config_for("new.log", 0);
	CHECK(logger_init(&replacement) == -1 && errno == EIO);
	CHECK(access("new.log", F_OK) == -1 && errno == ENOENT);
}

static void release_proof_failure(const char *kind)
{
	/* Global lock failures do not need a worker; only join/queue destroy
	 * tests create an asynchronous executor that may remain unjoined. */
	int asynchronous = !strcmp(kind, "join") ||
			   !strcmp(kind, "cond-destroy");
	logger_config_t cfg = config_for("old.log", asynchronous);
	int error = EIO;
	int lock_uncertain = 0;

	if (!strcmp(kind, "control")) {
		/* Capture only the Global control mutex, not an instance mutex. */
		capture_control = 1;
		fail_control_unlock = error;
		CHECK(logger_init(&cfg) == -1 && errno == error);
		CHECK(control_mutex != NULL);
		lock_uncertain = 1;
	} else {
		CHECK(logger_init(&cfg) == 0);
		if (!strcmp(kind, "reader")) {
			fail_rw_unlock = error;
			CHECK(logger_flush_status() == -1 && errno == error);
			lock_uncertain = 1;
		} else if (!strcmp(kind, "writer")) {
			fail_rw_unlock = error;
			CHECK(stop_status() == -1 && errno == error);
			lock_uncertain = 1;
		} else if (!strcmp(kind, "join")) {
			fail_join = error;
			CHECK(stop_status() == -1 && errno == error);
		} else if (!strcmp(kind, "cond-destroy")) {
			fail_cond_destroy = error;
			CHECK(stop_status() == -1 && errno == error);
		} else
			CHECK(!"unknown proof-failure scenario");
	}

	/* For all five paths the old instance was never fully released.
	 * Neither another generation nor another lock acquisition is legal. */
	CHECK(logger_process_object_count() == 1);
	pthread_t observer;
	CHECK(!pthread_create(&observer, NULL, verify_poisoned_controller,
				  &error));
	CHECK(!pthread_join(observer, NULL));
	CHECK(access("new.log", F_OK) == -1 && errno == ENOENT);

	int old_state;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state));
	CHECK(old_state == (lock_uncertain ?
				PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE));
	/* Production deliberately kept the object because its join failed.
	 * Test-only: reap the exact rejected worker after asserting BROKEN;
	 * never call logger_destroy again or reset the poisoned controller.
	 * This gives TSan an actual thread-happens-before edge at process exit. */
	if (!strcmp(kind, "join")) {
		CHECK(saw_rejected_join);
		CHECK(!__real_pthread_join(rejected_worker, NULL));
	}
	/* A failed unlock can still own a real lock; never try to clean up
	 * or reinitialize this test process. Process exit reclaims it. */
}

static void shutdown_error(int real_sync)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	LOG_INFO("dispose-even-on-failure");
	if (real_sync)
		atomic_store(&fail_sync, 1);
	else
		atomic_store(&fail_dispose, 1);
	CHECK(stop_status() == -1 && errno == EIO);
	CHECK(!logger_process_object_count());
	CHECK(!stop_status()); /* old failure is not a request to retry freed object */
	CHECK(!logger_init(&cfg));
	CHECK(!stop_status());
}
static void contract(void)
{
	CHECK(logger_flush_status() == -1 && errno == ENODEV);
	CHECK(logger_reopen() == -1 && errno == ENODEV);
	CHECK(logger_init(NULL) == -1 && errno == EINVAL);
	CHECK(logger_flush_status() == -1 && errno == ENODEV);
	LOG_INFO("bootstrap-after-failed-create");
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	unsigned n = atomic_load(&create_count);
	CHECK(logger_init(NULL) == -1 && errno == EALREADY);
	CHECK(atomic_load(&create_count) == (int)n);
	errno = ERANGE;
	LOG_INFO("keeps-errno");
	CHECK(errno == ERANGE);
	logger_set_level(LOGGER_DEBUG);
	CHECK(errno == ERANGE);
	CHECK(!logger_flush_status() && errno == ERANGE);
	logger_metrics_t metrics;
	logger_get_global_metrics(&metrics);
	CHECK(errno == ERANGE);
	logger_diagnostics_t diagnostics;
	logger_get_global_diagnostics(&diagnostics);
	CHECK(errno == ERANGE);
	CHECK(diagnostics.state == LOGGER_STATE_RUNNING);
	CHECK(diagnostics.outputs == LOGGER_OUT_FILE);
	logger_file_metrics_t file_metrics;
	CHECK(logger_get_global_file_metrics(&file_metrics) == 0);
	CHECK(errno == ERANGE);
	CHECK(!file_metrics.failed_write_operations);
	logger_set_level((logger_level_t)-1);
	CHECK(errno == EINVAL);
	CHECK(!stop_status());
	all_rejected(ESHUTDOWN);
	CHECK(!stop_status());
	atomic_store(&fail_create, 1);
	CHECK(logger_init(&cfg) == -1 && errno == ENOSPC);
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
	CHECK(!logger_init(&cfg));
	CHECK(!stop_status());
	CHECK(!logger_process_object_count());
}
static void child_status(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (stop_status() != -1 || errno != ECHILD)
			_exit(10);
		if (logger_init(&cfg) != -1 || errno != ECHILD)
			_exit(11);
		for (int i = 0; i < OP_COUNT; ++i) {
			call_result_t r = { .op = (enum operation)i };
			invoke(&r);
			if (r.error != ECHILD)
				_exit(20 + i);
		}
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	LOG_INFO("parent-unaffected");
	CHECK(!stop_status());
}

int main(int argc, char **argv)
{
	CHECK(argc >= 2);
	fixture_reset();
	if (!strcmp(argv[1], "generation")) {
		CHECK(argc == 3);
		generation(parse_operation(argv[2]), 0);
	} else if (!strcmp(argv[1], "bootstrap-generation"))
		generation(OP_WRITE, 1);
	else if (!strcmp(argv[1], "shutdown-generation"))
		shutdown_generation();
	else if (!strcmp(argv[1], "start-gate"))
		start_gate();
	else if (!strcmp(argv[1], "stop-gate"))
		stop_gate();
	else if (!strcmp(argv[1], "closed-readers")) {
		admission_only = 1;
		stop_gate();
	} else if (!strcmp(argv[1], "stop-during-start"))
		during_start_stop();
	else if (!strcmp(argv[1], "double-stop"))
		double_shutdown();
	else if (!strcmp(argv[1], "reentry")) {
		CHECK(argc == 3);
		recursion(argv[2]);
	} else if (!strcmp(argv[1], "acquire-error")) {
		CHECK(argc == 3);
		acquisition_error(argv[2]);
	} else if (!strcmp(argv[1], "release-proof")) {
		CHECK(argc == 3);
		release_proof_failure(argv[2]);
	} else if (!strcmp(argv[1], "proof-handoff")) {
		CHECK(argc == 3);
		unlock_shutdown_handoff(!strcmp(argv[2], "acquire-fail"));
	} else if (!strcmp(argv[1], "shutdown-error"))
		shutdown_error(0);
	else if (!strcmp(argv[1], "shutdown-fsync"))
		shutdown_error(1);
	else if (!strcmp(argv[1], "bootstrap-stop"))
		bootstrap_stop();
	else if (!strcmp(argv[1], "contract"))
		contract();
	else if (!strcmp(argv[1], "child"))
		child_status();
	else
		CHECK(!"unknown scenario");
	puts("global lifecycle regression passed");
	return 0;
}
