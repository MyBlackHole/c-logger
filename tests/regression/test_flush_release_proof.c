#define _GNU_SOURCE
#include "logger_internal.h"
#include "support.h"
#include <fcntl.h>
#include <sys/file.h>

/* Link wrappers inject one release-proof failure. All resources, worker I/O,
 * global lifetime pins, cancellation and subsequent lock calls remain real. */
static _Atomic(logger_t *) subject;
static _Atomic int pause_flush, at_progress, resume_flush;
static _Atomic int fail_emit_unlock, emit_poisoned, poisoned_reacquires;
static _Atomic int at_global_writer, stop_done, cancel_cleanup_ran;
static _Atomic int fail_sync, sync_calls, fail_progress_lock;
static _Thread_local int caller; /* 1: flush; 2: global shutdown */
static int use_global, stop_rc, stop_error;

logger_t *__real_logger_create(const logger_config_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_fsync(int);

logger_t *__wrap_logger_create(const logger_config_t *config)
{
	logger_t *created = __real_logger_create(config);
	if (created)
		subject = created;
	return created;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->progress_mu && caller == 1 &&
	    atomic_exchange(&pause_flush, 0)) {
		/* The API has admitted this flush while RUNNING, and the global
		 * variant already holds its real lifetime reader pin. */
		atomic_store(&at_progress, 1);
		wait_flag(&resume_flush);
	}
	if (subject && mu == &subject->progress_mu &&
	    atomic_exchange(&fail_progress_lock, 0))
		return EAGAIN;
	if (subject && mu == &subject->emit_mu && atomic_load(&emit_poisoned)) {
		atomic_fetch_add(&poisoned_reacquires, 1);
		/* Deliberately retain the real lock operation: the unfixed baseline
		 * hangs here and must be rejected by the runner's timeout. */
		fputs("unexpected reacquire of unproven emit_mu\n", stderr);
	}
	return __real_pthread_mutex_lock(mu);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->emit_mu &&
	    atomic_exchange(&fail_emit_unlock, 0)) {
		atomic_store(&emit_poisoned, 1);
		return EAGAIN; /* Do not actually release: ownership is unproven. */
	}
	return __real_pthread_mutex_unlock(mu);
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	if (caller == 2)
		atomic_store(&at_global_writer, 1);
	return __real_pthread_rwlock_wrlock(rw);
}

int __wrap_fsync(int fd)
{
	atomic_fetch_add(&sync_calls, 1);
	if (atomic_exchange(&fail_sync, 0)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void cancelled_flush_cleanup(void *unused)
{
	(void)unused;
	/* Cancellation must not escape while an explicit scope is still busy.
	 * For Global, completion of the waiting shutdown below also proves the
	 * reader pin was released before this cleanup ran. */
	CHECK(!logger_scope_busy());
	CHECK(logger_get_state(subject) == LOGGER_STATE_STOPPING);
	atomic_store(&cancel_cleanup_ran, 1);
}

static void *flush_thread(void *unused)
{
	(void)unused;
	caller = 1;
	int rc, error;
	pthread_cleanup_push(cancelled_flush_cleanup, NULL);
	rc = use_global ? logger_flush_status() :
			  logger_flush_instance_status(subject);
	error = errno;
	CHECK(rc == -1 && error == EAGAIN);
	pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}

static void *shutdown_thread(void *unused)
{
	(void)unused;
	caller = 2;
	stop_rc = logger_shutdown_status();
	stop_error = errno;
	atomic_store(&stop_done, 1);
	return NULL;
}

static logger_config_t config(int asynchronous)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = asynchronous;
	c.queue_capacity = 8;
	return c;
}

static void release_failure(int global, int cancel)
{
	use_global = global;
	logger_config_t c = config(1);
	if (global)
		CHECK(logger_init(&c) == 0);
	else
		CHECK(logger_create(&c) != NULL);
	CHECK(subject);

	atomic_store(&pause_flush, 1);
	pthread_t flusher, stopper;
	CHECK(!pthread_create(&flusher, NULL, flush_thread, NULL));
	wait_flag(&at_progress);
	atomic_store(&fail_emit_unlock, 1);
	if (global)
		logger_global_write(LOGGER_INFO, "proof", NULL, 0, NULL, "record");
	else
		logger_log(subject, LOGGER_INFO, "proof", NULL, 0, NULL, "record");
	wait_flag(&subject->synchronization_error);
	CHECK(atomic_load(&subject->synchronization_error) == EAGAIN);
	/* Test-only join proves this exact worker ended. Production must still
	 * retain the object because its emit lock could not be released. */
	CHECK(!pthread_join(subject->worker, NULL));
	CHECK(atomic_load(&subject->async_completed) == 1);
	int before = atomic_load(&sync_calls);
	if (global) {
		CHECK(!pthread_create(&stopper, NULL, shutdown_thread, NULL));
		wait_flag(&at_global_writer);
		CHECK(!atomic_load(&stop_done));
	}
	if (cancel)
		CHECK(!pthread_cancel(flusher));
	atomic_store(&resume_flush, 1);
	void *answer;
	CHECK(!pthread_join(flusher, &answer));
	CHECK(answer == (cancel ? PTHREAD_CANCELED : NULL));
	CHECK(atomic_load(&cancel_cleanup_ran) == cancel);
	CHECK(!atomic_load(&poisoned_reacquires));
	CHECK(atomic_load(&sync_calls) == before);
	if (global) {
		CHECK(!pthread_join(stopper, NULL));
		CHECK(stop_rc == -1 && stop_error == EAGAIN);
		CHECK(logger_init(&c) == -1 && errno == EAGAIN);
	} else {
		CHECK(logger_destroy_status(subject) == -1 && errno == EAGAIN);
	}
	CHECK(logger_process_object_count() == 1);
	int lease = open("out.log.logger.lock", O_RDWR | O_CLOEXEC);
	CHECK(lease >= 0);
	CHECK(flock(lease, LOCK_EX | LOCK_NB) == -1 &&
	      (errno == EAGAIN || errno == EWOULDBLOCK));
	CHECK(close(lease) == 0);
	/* Keep the retired allocation rooted until exit. Only fixture names are
	 * removed after all assertions; no successor owner is attempted afterward.
	 * Never retry teardown or reset the poisoned mutex for test cleanup. */
}

static void ordinary_error(int wait_error)
{
	logger_config_t c = config(wait_error);
	CHECK(logger_create(&c) != NULL);
	int expected;
	if (wait_error) {
		atomic_store(&fail_progress_lock, 1);
		expected = EAGAIN;
	} else {
		atomic_store(&fail_sync, 1);
		CHECK(logger_log_sync_status(subject, LOGGER_INFO, "io", NULL, 0,
					     NULL, "must still be synced") == -EIO);
		expected = EIO;
	}
	int before = atomic_load(&sync_calls);
	CHECK(logger_flush_instance_status(subject) == -1 && errno == expected);
	CHECK(!atomic_load(&subject->synchronization_error));
	CHECK(atomic_load(&sync_calls) > before);
	CHECK(wait_error ? logger_destroy_status(subject) == 0 :
			   logger_destroy_status(subject) == -1 && errno == EIO);
	subject = NULL;
	CHECK(logger_process_object_count() == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char directory[] = "/tmp/logger-flush-proof-XXXXXX";
	enter_temp(directory);
	if (!strcmp(argv[1], "explicit-proof"))
		release_failure(0, 0);
	else if (!strcmp(argv[1], "explicit-cancel"))
		release_failure(0, 1);
	else if (!strcmp(argv[1], "global-proof"))
		release_failure(1, 0);
	else if (!strcmp(argv[1], "global-cancel"))
		release_failure(1, 1);
	else if (!strcmp(argv[1], "io-error"))
		ordinary_error(0);
	else if (!strcmp(argv[1], "wait-error"))
		ordinary_error(1);
	else
		CHECK(!"unknown scenario");
	leave_temp(directory);
	return 0;
}
