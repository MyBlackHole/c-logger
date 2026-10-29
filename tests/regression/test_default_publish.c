#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "logger_lockdep.h"

/* 读锁真正释放以后才报告失败；写方不得用旧票据覆盖随后发布的 BROKEN。 */
static _Thread_local int poisoned_reader;
static _Atomic int reader_released, return_reader, reader_done;
static _Atomic int writer_paused, return_writer;
static int fail_writer;
static int setter_result;
static logger_t *a, *b;

int __real_pthread_rwlock_unlock(pthread_rwlock_t *rw);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *rw);

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
	int rc = __real_pthread_rwlock_unlock(rw);
	if (!rc && poisoned_reader) {
		poisoned_reader = 0;
		atomic_store(&reader_released, 1);
		wait_flag(&return_reader);
		return EIO;
	}
	return rc;
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	if (fail_writer) {
		atomic_store(&writer_paused, 1);
		wait_flag(&return_writer);
		return EAGAIN;
	}
	return __real_pthread_rwlock_wrlock(rw);
}

#if LOGGER_ENABLE_LOCKDEP
void __real_logger_lockdep_assert_held(enum logger_lock_class class_id,
				       const void *lock);
void __wrap_logger_lockdep_assert_held(enum logger_lock_class class_id,
				       const void *lock)
{
	__real_logger_lockdep_assert_held(class_id, lock);
	if (class_id == LOGGER_LOCK_GLOBAL_LIFETIME &&
	    lock == &logger_runtime_current()->global_lifetime_lock &&
	    atomic_load(&reader_released)) {
		/* 真实生产中此断言被编译为空。精确 CAS 窗口仅由支持库验收。 */
		atomic_store(&writer_paused, 1);
		wait_flag(&return_writer);
	}
}
#endif /* LOGGER_ENABLE_LOCKDEP */

static void *reader(void *unused)
{
	(void)unused;
	poisoned_reader = 1;
	errno = EDOM;
	CHECK(LOG_INFO("OLD_READER") == -EIO);
	CHECK(errno == EDOM);
	atomic_store(&reader_done, 1);
	return NULL;
}

static void *writer(void *unused)
{
	(void)unused;
	setter_result = logger_default_set(a, b);
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	fail_writer = !strcmp(argv[1], "writer-fail-poison");
	CHECK(fail_writer || !strcmp(argv[1], "publish-poison"));
#if !LOGGER_ENABLE_LOCKDEP
	CHECK(fail_writer);
#endif
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.async_mode = 0;
	CHECK(logger_create(&cfg, &a) == 0);
	CHECK(logger_create(&cfg, &b) == 0);
	/* 初次绑定不应注入写锁错误。 */
	int inject = fail_writer;
	fail_writer = 0;
	CHECK(logger_default_set(NULL, a) == 0);
	fail_writer = inject;
	pthread_t read_thread, write_thread;
	CHECK(pthread_create(&read_thread, NULL, reader, NULL) == 0);
	wait_flag(&reader_released);
	CHECK(pthread_create(&write_thread, NULL, writer, NULL) == 0);
	wait_flag(&writer_paused);
	atomic_store(&return_reader, 1);
	wait_flag(&reader_done);
	atomic_store(&return_writer, 1);
	CHECK(pthread_join(read_thread, NULL) == 0);
	CHECK(pthread_join(write_thread, NULL) == 0);
	CHECK(setter_result == (fail_writer ? -EAGAIN : -EIO));
	CHECK(logger_runtime_current()->global_logger == a);
	CHECK(atomic_load(&a->default_borrowed));
	CHECK(!atomic_load(&b->default_borrowed));
	CHECK(logger_default_set(a, NULL) == -EIO);
	CHECK(logger_destroy(&a) == -EBUSY && a);
	/* 新候选没有被发布、借用确实归还，因此宿主可以真实销毁。 */
	CHECK(logger_destroy(&b) == 0 && !b);
	CHECK(logger_process_object_count() == 1);
	printf("DEFAULT_PUBLISH_PASS=%s retained=1\n", argv[1]);
	return 0;
}
