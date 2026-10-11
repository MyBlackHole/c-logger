#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"

/* 故障注入只作用于触发 API 的测试线程，不干扰 worker 的真实锁操作。 */
static _Thread_local pthread_mutex_t *fail_mutex;
static _Thread_local pthread_mutex_t *failed_mutex;
static _Thread_local pthread_mutex_t *fail_unlock_mutex;
static _Thread_local pthread_mutex_t *captured_mutex;
static _Thread_local pthread_cond_t *fail_signal_cv;
static _Thread_local int capture_lock_after;
static _Thread_local int fail_unlock_once;
static _Thread_local int fail_signal_once;
static _Thread_local int lock_failed;
static _Thread_local int unlock_failed;
static _Thread_local int signal_failed;
static _Thread_local int invalid_unlock;

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_cond_signal(pthread_cond_t *);

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (capture_lock_after > 0 && --capture_lock_after == 0) {
		captured_mutex = mu;
		fail_unlock_mutex = mu;
	}
	if (fail_mutex == mu) {
		fail_mutex = NULL;
		failed_mutex = mu;
		lock_failed = 1;
		return EAGAIN;
	}
	int rc = __real_pthread_mutex_lock(mu);
	if (!rc && failed_mutex == mu)
		failed_mutex = NULL;
	return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (mu == failed_mutex) {
		failed_mutex = NULL;
		invalid_unlock = 1;
		return EPERM;
	}
	if (mu == fail_unlock_mutex && fail_unlock_once) {
		fail_unlock_once = 0;
		unlock_failed = 1;
		return EAGAIN;
	}
	return __real_pthread_mutex_unlock(mu);
}

int __wrap_pthread_cond_signal(pthread_cond_t *cv)
{
	if (cv == fail_signal_cv && fail_signal_once) {
		fail_signal_once = 0;
		signal_failed = 1;
		return EIO;
	}
	return __real_pthread_cond_signal(cv);
}

static int test_normal_lock_path(void)
{
	char dir[] = "/tmp/logger-lock-normal-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 0;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "lock-normal", NULL,
					0, NULL, "normal-lock-path");
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	int bytes = (int)file_size("out.log");
	int destroy_rc = logger_destroy_status(logger);
	leave_temp(dir);
	CHECK(rc == 0 && bytes > 0 && metrics.emitted_records == 1 &&
	      metrics.failed_records == 0 && destroy_rc == 0);
	return 0;
}

static int test_emit_lock_failure(void)
{
	char dir[] = "/tmp/logger-lock-emit-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 0;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);

	fail_mutex = &logger->emit_mu;
	errno = 0;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "lock-failure", NULL,
					0, NULL, "must-not-be-written");
	int error = errno;
	int bytes = (int)file_size("out.log");
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	int had_invalid_unlock = invalid_unlock;
	failed_mutex = NULL;
	int destroy_rc = logger_destroy_status(logger);
	(void)destroy_rc;
	leave_temp(dir);

	if (!lock_failed || rc != -EAGAIN || error != EAGAIN || bytes != 0 ||
	    had_invalid_unlock || metrics.emitted_records != 0 ||
	    metrics.failed_records != 1 || metrics.first_error != EAGAIN) {
		fprintf(stderr,
			"emit lock failure: lock=%d rc=%d errno=%d bytes=%d "
			"invalid_unlock=%d emitted=%llu failed=%llu first_error=%d\n",
			lock_failed, rc, error, bytes, had_invalid_unlock,
			(unsigned long long)metrics.emitted_records,
			(unsigned long long)metrics.failed_records,
			metrics.first_error);
		return 1;
	}
	return 0;
}

static int test_emit_unlock_failure(void)
{
	char dir[] = "/tmp/logger-lock-unlock-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 0;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);

	lock_failed = 0;
	unlock_failed = 0;
	invalid_unlock = 0;
	captured_mutex = NULL;
	capture_lock_after = 1;
	fail_unlock_once = 1;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "unlock-failure",
					NULL, 0, NULL, "write-may-have-committed");
	int error = errno;
	int bytes = (int)file_size("out.log");
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	int destroy_rc = logger_destroy_status(logger);
	int destroy_error = errno;
	leave_temp(dir);

	if (!captured_mutex || !unlock_failed || lock_failed || invalid_unlock ||
	    rc != -EAGAIN || error != EAGAIN || bytes <= 0 ||
	    metrics.emitted_records != 0 || metrics.failed_records != 1 ||
	    metrics.first_error != EAGAIN || destroy_rc != -1 ||
	    destroy_error != EAGAIN) {
		fprintf(stderr,
			"emit unlock failure: captured=%d unlock=%d lock=%d "
			"invalid_unlock=%d rc=%d errno=%d bytes=%d emitted=%llu "
			"failed=%llu first_error=%d destroy=%d destroy_errno=%d\n",
			captured_mutex != NULL, unlock_failed, lock_failed,
			invalid_unlock, rc, error, bytes,
			(unsigned long long)metrics.emitted_records,
			(unsigned long long)metrics.failed_records,
			metrics.first_error, destroy_rc, destroy_error);
		return 1;
	}
	return 0;
}

static int wait_for_worker_sleep(logger_t *logger)
{
	for (int i = 0; i < 5000; ++i) {
		if (atomic_load_explicit(&logger->q.consumer_waiting,
					 memory_order_acquire))
			return 0;
		nap_ms();
	}
	return 1;
}

static int wait_for_async_completion(logger_t *logger)
{
	for (int i = 0; i < 5000; ++i) {
		if (atomic_load_explicit(&logger->async_completed,
					 memory_order_acquire) == 1)
			return 0;
		nap_ms();
	}
	return 1;
}

static int test_queue_notify_lock_failure(void)
{
	char dir[] = "/tmp/logger-queue-notify-lock-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 1;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);
	CHECK(wait_for_worker_sleep(logger) == 0);

	lock_failed = 0;
	invalid_unlock = 0;
	fail_mutex = &logger->q.wait_mu;
	logger_log(logger, LOGGER_INFO, "queue-lock-failure", NULL, 0, NULL,
		   "published-before-notify-error");
	int had_lock_failure = lock_failed;
	int had_invalid_unlock = invalid_unlock;
	fail_mutex = NULL;
	CHECK(wait_for_async_completion(logger) == 0);
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	int destroy_rc = logger_destroy_status(logger);
	int destroy_error = errno;
	int bytes = (int)file_size("out.log");
	int content_ok = file_contains("out.log", "published-before-notify-error");
	leave_temp(dir);

	CHECK(had_lock_failure && !had_invalid_unlock &&
	      metrics.first_error == EAGAIN &&
	      metrics.async_completed == 1 && destroy_rc == -1 &&
	      destroy_error == EAGAIN && bytes > 0 && content_ok);
	return 0;
}

static int test_queue_notify_signal_failure(void)
{
	char dir[] = "/tmp/logger-queue-notify-signal-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 1;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);
	CHECK(wait_for_worker_sleep(logger) == 0);

	signal_failed = 0;
	fail_signal_cv = &logger->q.wait_cv;
	fail_signal_once = 1;
	logger_log(logger, LOGGER_INFO, "queue-signal-failure", NULL, 0, NULL,
		   "published-before-signal-error");
	fail_signal_cv = NULL;
	CHECK(wait_for_async_completion(logger) == 0);
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	uint64_t force_wakes = logger_queue_force_wake_signals(&logger->q);
	int destroy_rc = logger_destroy_status(logger);
	int destroy_error = errno;
	int bytes = (int)file_size("out.log");
	int content_ok = file_contains("out.log", "published-before-signal-error");
	leave_temp(dir);

	CHECK(signal_failed && force_wakes == 1 && metrics.first_error == EIO &&
	      metrics.async_completed == 1 && destroy_rc == -1 &&
	      destroy_error == EIO && bytes > 0 && content_ok);
	return 0;
}

static int test_force_wake_lock_failure(void)
{
	char dir[] = "/tmp/logger-force-wake-lock-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 1;
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&c);
	CHECK(logger);
	CHECK(wait_for_worker_sleep(logger) == 0);

	lock_failed = 0;
	invalid_unlock = 0;
	fail_mutex = &logger->q.wait_mu;
	errno = 0;
	int rc = logger_destroy_status(logger);
	int error = errno;
	int had_lock_failure = lock_failed;
	int had_invalid_unlock = invalid_unlock;
	fail_mutex = NULL;
	unsigned objects = logger_process_object_count();
	int state = atomic_load_explicit(&logger->state, memory_order_acquire);
	leave_temp(dir);

	CHECK(had_lock_failure && !had_invalid_unlock && rc == -1 &&
	      error == EAGAIN && objects == 1 &&
	      state == LOGGER_STATE_STOPPING);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "normal"))
		return test_normal_lock_path();
	if (!strcmp(argv[1], "emit"))
		return test_emit_lock_failure();
	if (!strcmp(argv[1], "emit-unlock"))
		return test_emit_unlock_failure();
	if (!strcmp(argv[1], "queue-notify"))
		return test_queue_notify_lock_failure();
	if (!strcmp(argv[1], "queue-signal"))
		return test_queue_notify_signal_failure();
	if (!strcmp(argv[1], "force-wake"))
		return test_force_wake_lock_failure();
	CHECK(!"unknown lock-failure scenario");
	return 2;
}
