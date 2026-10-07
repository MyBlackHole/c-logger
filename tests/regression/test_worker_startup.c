#define _GNU_SOURCE
#include "logger_internal.h"
#include "support.h"

/* Test-only link wrappers. EIO is synthetic; cond_wait keeps the mutex held.
 * No object is freed while the worker or an external borrower can reach it. */
enum scenario {
	NORMAL,
	SYNC,
	CREATE_ERROR,
	WORKER_FIRST,
	WORKER_LIVE,
	JOIN_ERROR,
	DESTROY_ERROR,
	CREATOR_FIRST,
	GLOBAL_WORKER_FIRST
};

static enum scenario selected;
static logger_t *candidate;
static pthread_t worker;
static void *(*start_worker)(void *);
static void *worker_arg;
static _Thread_local int is_worker;
static _Atomic int worker_returned;
static _Atomic int allow_start;
static _Atomic int allow_exit;
static unsigned create_calls;
static unsigned join_calls;
static unsigned joined;
static unsigned freed;
static unsigned destroy_errors;

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			 void *(*start)(void *), void *arg);
int __real_pthread_join(pthread_t thread, void **result);
int __real_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu);
int __real_pthread_cond_destroy(pthread_cond_t *cv);
void __real_free(void *ptr);

static void *worker_entry(void *unused)
{
	(void)unused;
	is_worker = 1;
	if (selected == CREATOR_FIRST)
		wait_flag(&allow_start);

	void *result = start_worker(worker_arg);

	atomic_store_explicit(&worker_returned, 1, memory_order_release);
	if (selected == WORKER_LIVE) {
		/* The trampoline deliberately retains a borrow until rollback joins. */
		wait_flag(&allow_exit);
		logger_t *l = worker_arg;
		CHECK(atomic_load_explicit(&l->state, memory_order_acquire) ==
		      LOGGER_STATE_STOPPING);
	}
	return result;
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			 void *(*start)(void *), void *arg)
{
	++create_calls;
	candidate = arg;
	if (selected == CREATE_ERROR)
		return EAGAIN;

	start_worker = start;
	worker_arg = arg;
	int rc = __real_pthread_create(thread, attr, worker_entry, NULL);

	if (rc)
		return rc;
	worker = *thread;
	if (selected != NORMAL && selected != CREATOR_FIRST)
		wait_flag(&worker_returned);
	return 0;
}

int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (is_worker && selected != NORMAL)
		return EIO;
	return __real_pthread_cond_wait(cv, mu);
}

int __wrap_pthread_join(pthread_t thread, void **result)
{
	CHECK(pthread_equal(thread, worker));
	++join_calls;
	if (selected == JOIN_ERROR)
		return EDEADLK;
	if (selected == WORKER_LIVE) {
		CHECK(atomic_load_explicit(&worker_returned,
					   memory_order_acquire));
		CHECK(!freed);
		atomic_store_explicit(&allow_exit, 1, memory_order_release);
	}
	int rc = __real_pthread_join(thread, result);

	if (!rc)
		++joined;
	return rc;
}

int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (selected == DESTROY_ERROR && candidate &&
	    cv == &candidate->q.wait_cv) {
		CHECK(joined == 1);
		++destroy_errors;
		return EBUSY;
	}
	return __real_pthread_cond_destroy(cv);
}

void __wrap_free(void *ptr)
{
	if (candidate && ptr == candidate) {
		if (selected != CREATE_ERROR)
			CHECK(joined == 1);
		++freed;
		candidate = NULL;
	}
	__real_free(ptr);
}

static enum scenario scenario_from_name(const char *name)
{
	static const char *const names[] = {
		"normal", "sync", "create-error", "worker-first", "worker-live",
		"join-error", "destroy-error", "creator-first", "global-worker-first"
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (!strcmp(name, names[i]))
			return (enum scenario)i;
	CHECK(!"unknown scenario");
	return NORMAL;
}

static void assert_clean_failure(void)
{
	CHECK(freed == 1 && candidate == NULL);
	CHECK(logger_process_object_count() == 0);
	CHECK(join_calls == (selected == CREATE_ERROR ? 0u : 1u));
	CHECK(joined == join_calls);
	CHECK(file_size("out.log") == 0);
}

static void test_healthy(logger_t *l)
{
	CHECK(l != NULL);
	CHECK(logger_get_state(l) == LOGGER_STATE_RUNNING);
	logger_log(l, LOGGER_INFO, "startup", NULL, 0, NULL, "healthy-output");
	CHECK(logger_flush_instance_status(l) == 0);

	logger_diagnostics_t d;

	logger_get_diagnostics(l, &d);
	CHECK(d.emitted_records == 1 && d.failed_records == 0);
	CHECK(d.first_error == 0);
	CHECK(d.enqueued == (selected == SYNC ? 0u : 1u));
	CHECK(d.async_completed == d.enqueued);
	CHECK(d.worker_running == (selected != SYNC));
	CHECK(logger_destroy_status(l) == 0);
	CHECK(logger_process_object_count() == 0);
	CHECK(create_calls == (selected == SYNC ? 0u : 1u));
	CHECK(joined == create_calls && join_calls == create_calls);
	CHECK(file_contains("out.log", "healthy-output"));
}

static void test_late_failure(logger_t *l)
{
	CHECK(l != NULL);
	CHECK(logger_get_state(l) == LOGGER_STATE_RUNNING);
	atomic_store_explicit(&allow_start, 1, memory_order_release);
	wait_flag(&worker_returned);
	CHECK(logger_get_state(l) == LOGGER_STATE_STOPPING);
	CHECK(atomic_load_explicit(&l->lifecycle_error,
				   memory_order_acquire) == EIO);

	/* Only calls entering after failure are tested. In-flight flush is #124. */
	errno = 0;
	logger_log(l, LOGGER_INFO, "startup", NULL, 0, NULL, "rejected");
	CHECK(errno == ESHUTDOWN);
	CHECK(logger_log_sync_status(l, LOGGER_INFO, "startup", NULL, 0,
				     NULL, "rejected") == -ESHUTDOWN);
	logger_diagnostics_t d;

	logger_get_diagnostics(l, &d);
	CHECK(!d.worker_running && d.enqueued == 0 && d.emitted_records == 0);
	CHECK(file_size("out.log") == 0);
	errno = 0;
	CHECK(logger_destroy_status(l) == -1 && errno == EIO);
	assert_clean_failure();
}

static void test_early_failure(logger_t *l, int error)
{
	CHECK(l == NULL);
	CHECK(error == (selected == CREATE_ERROR ? EAGAIN : EIO));
	CHECK(create_calls == 1);
	if (selected != CREATE_ERROR)
		CHECK(atomic_load_explicit(&worker_returned,
					   memory_order_acquire));

	if (selected == JOIN_ERROR || selected == DESTROY_ERROR) {
		CHECK(candidate != NULL && freed == 0 && join_calls == 1);
		CHECK(joined == (selected == DESTROY_ERROR ? 1u : 0u));
		CHECK(destroy_errors == joined);
		CHECK(logger_process_object_count() == 1);
		CHECK(atomic_load_explicit(&candidate->state,
					   memory_order_acquire) ==
		      LOGGER_STATE_STOPPING);
		CHECK(atomic_load_explicit(&candidate->lifecycle_error,
					   memory_order_acquire) == EIO);
		CHECK(file_size("out.log") == 0);
		/* Reap the test thread, not the retained library object. Static roots
		 * retain that tombstone; do not retry a partially torn-down object. */
		if (selected == JOIN_ERROR)
			CHECK(__real_pthread_join(worker, NULL) == 0);
		return;
	}
	assert_clean_failure();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	selected = scenario_from_name(argv[1]);
	char dir[] = "/tmp/logger-startup-XXXXXX";

	enter_temp(dir);
	logger_config_t config = LOGGER_DEFAULT_CONFIG();

	config.outputs = LOGGER_OUT_FILE;
	config.file_path = "out.log";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	config.queue_capacity = 8;
	config.async_mode = selected != SYNC;
	if (selected == GLOBAL_WORKER_FIRST) {
		errno = 0;
		CHECK(logger_init(&config) == -1 && errno == EIO);
		assert_clean_failure();
		CHECK(logger_shutdown_status() == 0);
	} else {
		errno = 0;
		logger_t *l = logger_create(&config);
		int error = errno;

		if (selected == NORMAL || selected == SYNC)
			test_healthy(l);
		else if (selected == CREATOR_FIRST)
			test_late_failure(l);
		else
			test_early_failure(l, error);
	}
	if (selected != JOIN_ERROR && selected != DESTROY_ERROR)
		leave_temp(dir);
	/* Retain the on-disk fixture too if the object deliberately owns its fd. */
	printf("worker startup: %s PASS (create=%u join=%u joined=%u freed=%u)\n",
	       argv[1], create_calls, join_calls, joined, freed);
	return 0;
}
