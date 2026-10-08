#define _GNU_SOURCE
#include "logger_internal.h"
#include "support.h"

/* All gates and injected failures are link-time test code, never runtime hooks.
 * The synthetic cond_wait EIO leaves its valid, non-robust mutex held. */
enum scenario {
	WAIT_FIRST, ERROR_FIRST, WAIT_GAP, MULTIPLE, SPURIOUS, LATER_SLOT,
	SPILL, NORMAL_GAP, NORMAL_DRAIN, BACKEND_ERROR, EMPTY_TARGET,
	GLOBAL, CANCEL
};

struct answer {
	int rc;
	int error;
	_Atomic int calls;
	_Atomic int done;
};

static enum scenario selected;
static logger_t *instance;
static void *(*worker_fn)(void *);
static void *worker_arg;
static pthread_t worker_id;
static _Thread_local int is_worker;
static _Thread_local int pause_source;
static _Thread_local int progress_owned;
static _Thread_local int wait_owned;
static _Thread_local struct answer *waiter;
static _Atomic int worker_waiting;
static _Atomic int release_worker;
static _Atomic int worker_returned;
static _Atomic int head_reserved;
static _Atomic int release_head;
static _Atomic int before_lock;
static _Atomic int release_waiter;
static _Atomic int notifier_attempt;
static _Atomic int error_broadcasts;
static _Atomic int producers_returned;
static unsigned joined;
static unsigned released;
static size_t joined_completed;
static uint64_t joined_emitted;

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			 void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __real_pthread_cond_broadcast(pthread_cond_t *);
char *__real_strrchr(const char *, int);
int __real_logger_file_writev(logger_file_t *, struct iovec *, int, size_t,
			      const struct timespec *, int);
void __real_free(void *);

static int healthy_worker(void)
{
	return selected == NORMAL_GAP || selected == NORMAL_DRAIN ||
	       selected == BACKEND_ERROR;
}

static int defer_waiter(void)
{
	return selected == ERROR_FIRST || selected == EMPTY_TARGET;
}

static void await_count(const _Atomic int *value, int expected)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (atomic_load(value) >= expected)
			return;
		nap_ms();
	}
	CHECK(!"test coordination or completion deadline exceeded");
}

static void *worker_entry(void *unused)
{
	(void)unused;
	is_worker = 1;
	if (selected == NORMAL_DRAIN) {
		atomic_store(&worker_waiting, 1);
		wait_flag(&release_worker);
	}
	void *result = worker_fn(worker_arg);

	atomic_store(&worker_returned, 1);
	return result;
}

int __wrap_pthread_create(pthread_t *id, const pthread_attr_t *attr,
			 void *(*fn)(void *), void *arg)
{
	instance = arg;
	worker_fn = fn;
	worker_arg = arg;
	int rc = __real_pthread_create(id, attr, worker_entry, NULL);

	if (!rc)
		worker_id = *id;
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (waiter && mu == &instance->progress_mu && defer_waiter() &&
	    !atomic_exchange(&before_lock, 1))
		wait_flag(&release_waiter);
	if (is_worker && mu == &instance->progress_mu &&
	    atomic_load(&instance->lifecycle_error))
		atomic_store(&notifier_attempt, 1);
	int rc = __real_pthread_mutex_lock(mu);

	if (!rc && is_worker) {
		if (mu == &instance->progress_mu)
			progress_owned = 1;
		if (mu == &instance->q.wait_mu)
			wait_owned = 1;
	}
	return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	int rc = __real_pthread_mutex_unlock(mu);

	if (!rc && is_worker) {
		if (mu == &instance->progress_mu)
			progress_owned = 0;
		if (mu == &instance->q.wait_mu)
			wait_owned = 0;
	}
	return rc;
}

int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (is_worker && cv == &instance->q.wait_cv &&
	    !atomic_exchange(&worker_waiting, 1)) {
		CHECK(wait_owned && !progress_owned);
		wait_flag(&release_worker);
		return healthy_worker() ? 0 : EIO;
	}
	if (waiter && cv == &instance->progress_cv) {
		int previous = atomic_fetch_add(&waiter->calls, 1);

		if (selected == SPURIOUS && !previous)
			return 0; /* A wake without progress must recheck the predicate. */
		if (selected == WAIT_GAP && !previous)
			wait_flag(&release_waiter); /* Still owns progress_mu. */
	}
	return __real_pthread_cond_wait(cv, mu);
}

int __wrap_pthread_cond_broadcast(pthread_cond_t *cv)
{
	if (is_worker && cv == &instance->progress_cv &&
	    atomic_load(&instance->lifecycle_error)) {
		CHECK(progress_owned && !wait_owned);
		atomic_fetch_add(&error_broadcasts, 1);
	}
	return __real_pthread_cond_broadcast(cv);
}

char *__wrap_strrchr(const char *s, int c)
{
	if (pause_source && !atomic_exchange(&head_reserved, 1))
		wait_flag(&release_head);
	return __real_strrchr(s, c);
}

int __wrap_logger_file_writev(logger_file_t *f, struct iovec *vec, int count,
			     size_t length, const struct timespec *ts, int sync)
{
	if (selected == BACKEND_ERROR)
		return -ENOSPC;
	return __real_logger_file_writev(f, vec, count, length, ts, sync);
}

int __wrap_pthread_join(pthread_t id, void **result)
{
	CHECK(pthread_equal(id, worker_id));
	if (selected == NORMAL_DRAIN) {
		CHECK(!atomic_load(&instance->running));
		CHECK(atomic_load(&instance->q.enqueue_pos) == 5);
		atomic_store(&release_worker, 1);
	}
	int rc = __real_pthread_join(id, result);

	if (!rc) {
		++joined;
		joined_completed = instance->completed_pos;
		joined_emitted = atomic_load(&instance->emitted_records);
	}
	return rc;
}

void __wrap_free(void *ptr)
{
	if (instance && ptr == instance) {
		CHECK(joined == 1);
		++released;
		instance = NULL;
	}
	__real_free(ptr);
}

static void *produce(void *arg)
{
	char text[1024];
	int head = arg == NULL;

	pause_source = head;
	if (selected == SPILL && head) {
		memset(text, 'x', sizeof(text) - 1);
		text[sizeof(text) - 1] = 0;
	} else {
		memcpy(text, "pending-record", sizeof("pending-record"));
	}
	logger_log(instance, LOGGER_INFO, "failure", "test/source.c", 1,
		   "produce", "%s", text);
	pause_source = 0;
	atomic_fetch_add(&producers_returned, 1);
	return NULL;
}

static void finish_waiter(void *arg)
{
	struct answer *a = arg;

	atomic_store(&a->done, 1);
}

static void *flush(void *arg)
{
	struct answer *a = arg;

	waiter = a;
	pthread_cleanup_push(finish_waiter, a);
	errno = 0;
	a->rc = selected == GLOBAL ? logger_flush_status() :
		logger_flush_instance_status(instance);
	a->error = errno;
	pthread_testcancel();
	pthread_cleanup_pop(1);
	return NULL;
}

static void await_later_slot(void)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (atomic_load(&instance->q.slots[1].seq) == 2)
			return;
		nap_ms();
	}
	CHECK(!"later slot did not publish behind the reserved head");
}

static void test_failure_or_gap(void)
{
	unsigned count = selected == MULTIPLE || selected == GLOBAL ? 4 : 1;
	unsigned producers = selected == EMPTY_TARGET ? 0 :
		selected == LATER_SLOT ? 2 : 1;
	pthread_t producer[2];
	pthread_t flushers[4];
	struct answer answers[4] = { 0 };

	if (producers) {
		CHECK(!__real_pthread_create(&producer[0], NULL, produce, NULL));
		wait_flag(&head_reserved);
		CHECK(atomic_load(&instance->q.enqueue_pos) == 1);
		CHECK(atomic_load(&instance->q.slots[0].seq) == 0);
		CHECK(atomic_load(&instance->enqueued) == 0);
	}
	if (producers == 2) {
		CHECK(!__real_pthread_create(&producer[1], NULL, produce, &count));
		await_later_slot();
		CHECK(atomic_load(&instance->q.enqueue_pos) == 2);
	}
	if (selected == SPILL)
		CHECK(logger_queue_spill_in_use(&instance->q) == 1);

	for (unsigned i = 0; i < count; ++i) {
		CHECK(!__real_pthread_create(&flushers[i], NULL, flush, &answers[i]));
		if (defer_waiter())
			wait_flag(&before_lock); /* Admission and watermark already read. */
		else
			await_count(&answers[i].calls, selected == SPURIOUS ? 2 : 1);
		CHECK(!atomic_load(&answers[i].done));
	}
	if (selected == CANCEL)
		CHECK(!pthread_cancel(flushers[0]));

	atomic_store(&release_worker, 1);
	if (healthy_worker()) {
		atomic_store(&release_head, 1);
	} else {
		if (selected == WAIT_GAP) {
			wait_flag(&notifier_attempt);
			CHECK(!atomic_load(&error_broadcasts));
			atomic_store(&release_waiter, 1);
		}
		wait_flag(&worker_returned);
		CHECK(atomic_load(&instance->lifecycle_error) == EIO);
		if (defer_waiter())
			atomic_store(&release_waiter, 1);
	}

	/* Failure must release all waiters even while the producer still owns
	 * its reservation. Notification must not pretend to complete that slot. */
	for (unsigned i = 0; i < count; ++i) {
		void *result;

		wait_flag(&answers[i].done);
		CHECK(!__real_pthread_join(flushers[i], &result));
		if (selected == CANCEL) {
			CHECK(result == PTHREAD_CANCELED);
		} else {
			int error = !healthy_worker() ? EIO :
				selected == BACKEND_ERROR ? ENOSPC : 0;

			CHECK(result == NULL);
			CHECK(answers[i].rc == (error ? -1 : 0));
			CHECK(answers[i].error == error);
		}
		if (defer_waiter())
			CHECK(atomic_load(&answers[i].calls) == 0);
	}
	atomic_store(&release_head, 1);
	for (unsigned i = 0; i < producers; ++i)
		CHECK(!__real_pthread_join(producer[i], NULL));
	CHECK(atomic_load(&producers_returned) == (int)producers);

	logger_diagnostics_t d;

	logger_get_diagnostics(instance, &d);
	CHECK(d.enqueued == producers && d.dropped_records == 0);
	if (!healthy_worker()) {
		CHECK(atomic_load(&error_broadcasts) == 1);
		CHECK(d.state == LOGGER_STATE_STOPPING && !d.worker_running);
		CHECK(d.async_completed == 0 && d.emitted_records == 0);
		CHECK(d.failed_records == 0 && d.queue_depth == producers);
		CHECK(file_size("out.log") == 0);
		if (selected == SPILL)
			CHECK(logger_queue_spill_in_use(&instance->q) == 1);
		errno = 0;
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == ESHUTDOWN); /* New calls follow admission, not wait. */
	} else {
		CHECK(d.state == LOGGER_STATE_RUNNING && d.worker_running);
		CHECK(d.async_completed == producers && d.queue_depth == 0);
		CHECK(atomic_load(&instance->lifecycle_error) == 0);
		CHECK(d.emitted_records == (selected == BACKEND_ERROR ? 0u : 1u));
		CHECK(d.failed_records == (selected == BACKEND_ERROR ? 1u : 0u));
		CHECK(!atomic_load(&error_broadcasts));
	}
}

static enum scenario scenario_from_name(const char *name)
{
	static const char *const names[] = {
		"wait-first", "error-first", "wait-gap", "multiple", "spurious",
		"later-slot", "spill", "normal-gap", "normal-drain", "backend-error",
		"empty-target", "global", "cancel"
	};

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (!strcmp(names[i], name))
			return (enum scenario)i;
	CHECK(!"unknown scenario");
	return WAIT_FIRST;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	selected = scenario_from_name(argv[1]);
	char dir[] = "/tmp/logger-worker-failure-XXXXXX";
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	enter_temp(dir);
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.detail = LOGGER_DETAIL_DEBUG;
	c.include_source = 1;
	c.queue_capacity = 8;
	c.async_mode = 1;
	if (selected == GLOBAL)
		CHECK(logger_init(&c) == 0);
	else {
		logger_t *l = logger_create(&c);

		CHECK(l != NULL && l == instance);
	}
	wait_flag(&worker_waiting);

	if (selected == NORMAL_DRAIN) {
		for (unsigned i = 0; i < 5; ++i)
			logger_log(instance, LOGGER_INFO, "drain", NULL, 0, NULL,
				   "record %u", i);
		CHECK(atomic_load(&instance->enqueued) == 5);
		CHECK(atomic_load(&instance->async_completed) == 0);
	} else {
		test_failure_or_gap();
	}

	/* External borrowers have all joined. Only now may the owner dispose. */
	int expected = !healthy_worker() ? EIO :
		selected == BACKEND_ERROR ? ENOSPC : 0;

	errno = 0;
	int rc = selected == GLOBAL ? logger_shutdown_status() :
		logger_destroy_status(instance);

	CHECK(rc == (expected ? -1 : 0) && errno == expected);
	CHECK(instance == NULL && joined == 1 && released == 1);
	CHECK(logger_process_object_count() == 0);
	if (selected == NORMAL_DRAIN)
		CHECK(joined_completed == 5 && joined_emitted == 5);
	leave_temp(dir);
	printf("worker failure: %s PASS (broadcasts=%d completed=%zu emitted=%llu)\n",
	       argv[1], atomic_load(&error_broadcasts), joined_completed,
	       (unsigned long long)joined_emitted);
	return 0;
}
