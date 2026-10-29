#define _GNU_SOURCE
#include <errno.h>
#include "default_fixture.h"
static logger_t *default_owner;
#include "logger_internal.h"
#include "support.h"
#include <fcntl.h>
#include <sys/file.h>

/*
 * 链接包装函数注入一次释放证明失败。全部资源、工作线程 I/O、
 * 全局生命周期保护、取消与后续锁调用仍是真实的。
 */
static _Atomic(logger_t *) subject;
static logger_t *owner; /* 唯一拥有者；subject 仅供并发观察。 */
static _Atomic int pause_flush;
static _Atomic int at_progress;
static _Atomic int resume_flush;
static _Atomic int fail_emit_unlock;
static _Atomic int emit_poisoned;
static _Atomic int poisoned_reacquires;
static _Atomic int at_global_writer;
static _Atomic int stop_done;
static _Atomic int cancel_cleanup_ran;
static _Atomic int fail_sync;
static _Atomic int sync_calls;
static _Atomic int fail_progress_lock;
static _Thread_local int caller; /* 1：刷新；2：全局关闭 */
static int use_global;
static int stop_rc;
static int stop_error;

int __real_logger_create(const logger_config_t *config, logger_t **out);
int __real_pthread_mutex_lock(pthread_mutex_t *mu);
int __real_pthread_mutex_unlock(pthread_mutex_t *mu);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *rw);
int __real_fsync(int fd);

int __wrap_logger_create(const logger_config_t *config, logger_t **out)
{
	int rc = __real_logger_create(config, out);

	if (!rc)
		subject = *out;
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->progress_mu && caller == 1 &&
	    atomic_exchange(&pause_flush, 0)) {
		/*
		 * API 已在 RUNNING 时接纳本次刷新，全局变体
		 * 也已持有其真实的生命周期读侧保护。
		 */
		atomic_store(&at_progress, 1);
		wait_flag(&resume_flush);
	}
	if (subject && mu == &subject->progress_mu &&
	    atomic_exchange(&fail_progress_lock, 0))
		return EAGAIN;
	if (subject && mu == &subject->emit_mu && atomic_load(&emit_poisoned)) {
		atomic_fetch_add(&poisoned_reacquires, 1);
		/*
		 * 有意保留真实锁操作：未修复基线会在此挂起，
		 * 必须由测试运行器超时拒绝。
		 */
		fputs("unexpected reacquire of unproven emit_mu\n", stderr);
	}
	return __real_pthread_mutex_lock(mu);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->emit_mu &&
	    atomic_exchange(&fail_emit_unlock, 0)) {
		atomic_store(&emit_poisoned, 1);
		return EAGAIN; /* 不实际释放：所有权未经证明。 */
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
	/*
	 * 显式作用域仍忙碌时，取消不得逃逸。
	 * 对于全局情形，下方等待关闭的完成也证明
	 * 此清理运行前读侧生命周期保护已释放。
	 */
	CHECK(!logger_scope_busy());
	logger_state_t state;

	CHECK(logger_get_state(subject, &state) == 0 &&
	      state == LOGGER_STATE_STOPPING);
	atomic_store(&cancel_cleanup_ran, 1);
}

static void *flush_thread(void *unused)
{
	(void)unused;
	caller = 1;
	int rc;
	int error;

	pthread_cleanup_push(cancelled_flush_cleanup, NULL);
	errno = EDOM;
	rc = use_global ? logger_default_flush() : logger_flush(subject);
	error = errno;
	CHECK(rc == -EAGAIN && error == EDOM);
	pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}

static void *shutdown_thread(void *unused)
{
	(void)unused;
	caller = 2;
	stop_rc = fixture_default_destroy(&default_owner);
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
		CHECK(fixture_default_create(&c, &default_owner) == 0);
	else
		CHECK(logger_create(&c, &owner) == 0);
	CHECK(subject);

	atomic_store(&pause_flush, 1);
	pthread_t flusher;
	pthread_t stopper;

	CHECK(!pthread_create(&flusher, NULL, flush_thread, NULL));
	wait_flag(&at_progress);
	atomic_store(&fail_emit_unlock, 1);
	if (global)
		logger_global_write(LOGGER_INFO, "proof", NULL, 0, NULL,
				    "record");
	else
		logger_log(subject, LOGGER_INFO, "proof", NULL, 0, NULL,
			   "record");
	wait_flag(&subject->synchronization_error);
	CHECK(atomic_load(&subject->synchronization_error) == EAGAIN);
	/*
	 * 仅供测试的线程回收证明此精确工作线程已结束。
	 * 生产库仍必须保留对象，因为其输出锁未能释放。
	 */
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
		CHECK(stop_rc == -EAGAIN && default_owner == NULL);
		CHECK(fixture_default_create(&c, &default_owner) == -EBUSY);
	} else {
		CHECK(logger_destroy(&owner) == -EAGAIN);
	}
	CHECK(logger_process_object_count() == 1);
	int lease = open("out.log.logger.lock", O_RDWR | O_CLOEXEC);

	CHECK(lease >= 0);
	CHECK(flock(lease, LOCK_EX | LOCK_NB) == -1 &&
	      (errno == EAGAIN || errno == EWOULDBLOCK));
	CHECK(close(lease) == 0);
	/*
	 * 保留退役分配的根引用直到退出。全部断言后只移除夹具名称；
	 * 之后不尝试建立后继所有者。
	 * 测试清理绝不重试拆除，也不重置已失效互斥锁。
	 */
}

static void ordinary_error(int wait_error)
{
	logger_config_t c = config(wait_error);

	CHECK(logger_create(&c, &owner) == 0);
	int expected;

	if (wait_error) {
		atomic_store(&fail_progress_lock, 1);
		expected = EAGAIN;
	} else {
		atomic_store(&fail_sync, 1);
		CHECK(logger_log_sync_status(subject, LOGGER_INFO, "io", NULL,
					     0, NULL,
					     "must still be synced") == -EIO);
		expected = EIO;
	}
	int before = atomic_load(&sync_calls);

	CHECK(logger_flush(subject) == -expected);
	CHECK(!atomic_load(&subject->synchronization_error));
	CHECK(atomic_load(&sync_calls) > before);
	CHECK(wait_error ? logger_destroy(&owner) == 0 :
			   logger_destroy(&owner) == -EIO);
	subject = NULL;
	CHECK(logger_process_object_count() == 0);
}

int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
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
