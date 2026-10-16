#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include <sys/mman.h>

/* 拥有者槽属于调用方。探针只观察真实交接、取消和系统资源回收。 */
static logger_t *owned;
static logger_t *subject;
static unsigned freed;
static int fail_join, fail_entry, fail_unmap;
static int cancel_create, cancel_destroy, reentry;
static _Thread_local int caller;
static _Atomic int cleanup_ran;
static logger_config_t config;

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			  void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
int __real_pthread_setcancelstate(int, int *);
int __real_logger_runtime_free(void *);
int __real_munmap(void *, size_t);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start)(void *), void *arg)
{
	if (start != logger_worker_main)
		return __real_pthread_create(thread, attr, start, arg);
	CHECK(owned == NULL); /* 构造尚未发布调用方所有权。 */
	subject = arg;
	int rc = __real_pthread_create(thread, attr, start, arg);
	if (!rc && caller && cancel_create)
		CHECK(pthread_cancel(pthread_self()) == 0);
	return rc;
}

int __wrap_pthread_join(pthread_t thread, void **result)
{
	if (!caller)
		return __real_pthread_join(thread, result);
	CHECK(owned == NULL); /* 不可回滚拆除开始之前已清空拥有者槽。 */
	if (reentry) {
		logger_t *alias = subject;
		errno = EDOM;
		CHECK(logger_destroy(&alias) == -EDEADLK);
		CHECK(alias == subject && errno == EDOM);
	}
	if (cancel_destroy)
		CHECK(pthread_cancel(pthread_self()) == 0);
	int rc = __real_pthread_join(thread, result);
	if (!rc && fail_join)
		return EIO; /* 只向库报告失败；测试自己的线程必须真正回收。 */
	return rc;
}

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (caller && state == PTHREAD_CANCEL_DISABLE && fail_entry) {
		fail_entry = 0;
		return EIO;
	}
	if (caller && state == PTHREAD_CANCEL_ENABLE && cancel_destroy)
		CHECK(owned == NULL);
	return __real_pthread_setcancelstate(state, old);
}

int __wrap_munmap(void *base, size_t bytes)
{
	uintptr_t address = (uintptr_t)subject;
	if (subject && (uintptr_t)base <= address &&
	    address - (uintptr_t)base < bytes && fail_unmap) {
		fail_unmap = 0;
		errno = EIO;
		return -1;
	}
	return __real_munmap(base, bytes);
}

int __wrap_logger_runtime_free(void *ptr)
{
	int is_subject = subject && ptr == subject;
	if (is_subject)
		CHECK(owned == NULL);
	int rc = __real_logger_runtime_free(ptr);
	if (is_subject && !rc) {
		++freed;
		subject = NULL;
	}
	return rc;
}

static void cleanup_owner(void *unused)
{
	(void)unused;
	CHECK(owned == NULL);
	CHECK(logger_destroy(&owned) == 0);
	atomic_store(&cleanup_ran, 1);
}

static void *cancelled_caller(void *unused)
{
	(void)unused;
	caller = 1;
	pthread_cleanup_push(cleanup_owner, NULL);
	if (cancel_create) {
		(void)logger_create(&config, &owned);
		CHECK(0); /* 待定取消必须在候选发布之前交付。 */
	} else {
		CHECK(logger_destroy(&owned) == 0);
		pthread_testcancel();
		CHECK(0);
	}
	pthread_cleanup_pop(0);
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char directory[] = "/tmp/logger-owner-XXXXXX";
	enter_temp(directory);
	CHECK(logger_process_init() == 0);
	config = LOGGER_DEFAULT_CONFIG();
	config.outputs = LOGGER_OUT_FILE;
	config.file_path = "out.log";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	config.queue_capacity = 8;
	int io_error = !strcmp(argv[1], "io-error");
	cancel_create = !strcmp(argv[1], "cancel-create");
	cancel_destroy = !strcmp(argv[1], "cancel-destroy");
	if (io_error) {
		config.file_path = "/dev/full";
		config.async_mode = 0;
	}
	if (!cancel_create) {
		CHECK(logger_create(&config, &owned) == 0);
		subject = owned;
		LOGGER_INFO(owned, "owner", "OWNER_RECORD");
	}
	if (cancel_create || cancel_destroy) {
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, cancelled_caller, NULL) ==
		      0);
		void *result;
		CHECK(pthread_join(thread, &result) == 0);
		CHECK(result == PTHREAD_CANCELED);
		CHECK(atomic_load(&cleanup_ran) == 1);
		CHECK(owned == NULL && freed == 1);
	} else {
		caller = 1;
		if (!strcmp(argv[1], "entry-error")) {
			fail_entry = 1;
			errno = ERANGE;
			CHECK(logger_destroy(&owned) == -EIO);
			CHECK(owned == subject && owned && !freed);
			CHECK(errno == ERANGE);
		}
		fail_join = !strcmp(argv[1], "join-error");
		fail_unmap = !strcmp(argv[1], "unmap-error");
		reentry = !strcmp(argv[1], "reentry");
		int retained = fail_join || fail_unmap;
		int expected = io_error ? -ENOSPC : (retained ? -EIO : 0);
		errno = ERANGE;
		CHECK(logger_destroy(&owned) == expected);
		CHECK(owned == NULL && errno == ERANGE);
		CHECK(freed == (retained ? 0u : 1u));
		CHECK(logger_destroy_receipt_read() == !retained);
		CHECK(logger_destroy(&owned) == 0); /* 不再次操作旧分配。 */
		if (retained) {
			CHECK(logger_runtime_current()->allocation_count > 0);
			leave_temp(directory);
			printf("LIFECYCLE_OWNER_PASS=%s retained=1\n", argv[1]);
			return 0;
		}
	}
	CHECK(logger_runtime_current()->allocation_count == 0);
	CHECK(logger_process_object_count() == 0);
	if (io_error) {
		CHECK(chdir("/") == 0);
		CHECK(rmdir(directory) == 0);
	} else {
		leave_temp(directory);
	}
	printf("LIFECYCLE_OWNER_PASS=%s\n", argv[1]);
	return 0;
}
