#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include <errno.h>

/* 包装器只注入选定边界；正常操作仍由真实生产或同源支持归档完成。 */
static logger_t *owner;
static int active;
static int sync_error;
static int reopen_error;
static int setup_error;
static int restore_error;
static int emit_lock_error;
static int emit_unlock_error;
static int read_error;
static int read_unlock_error;
static unsigned int sync_calls;
static unsigned int reopen_calls;
static unsigned int cancel_calls;

int __real_fsync(int fd);
int __real_logger_file_reopen(struct logger_file *file);
int __real_pthread_setcancelstate(int state, int *old);
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *lock);

int __wrap_fsync(int fd)
{
	if (active) {
		++sync_calls;
		if (sync_error) {
			errno = sync_error;
			return -1;
		}
	}
	return __real_fsync(fd);
}

int __wrap_logger_file_reopen(struct logger_file *file)
{
	if (active) {
		++reopen_calls;
		if (reopen_error) {
			errno = reopen_error;
			return -1;
		}
	}
	return __real_logger_file_reopen(file);
}

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (active && ++cancel_calls == 1 && setup_error)
		return setup_error;
	int rc = __real_pthread_setcancelstate(state, old);

	/* 先执行真实恢复再报告模拟错误，不能破坏宿主线程的取消状态。 */
	if (active && !rc && state == PTHREAD_CANCEL_ENABLE && restore_error)
		return restore_error;
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (active && mutex == &owner->emit_mu && emit_lock_error)
		return emit_lock_error;
	return __real_pthread_mutex_lock(mutex);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_unlock(mutex);

	if (active && !rc && mutex == &owner->emit_mu && emit_unlock_error)
		return emit_unlock_error;
	return rc;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	if (active && read_error)
		return read_error;
	return __real_pthread_rwlock_rdlock(lock);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	int rc = __real_pthread_rwlock_unlock(lock);

	if (active && !rc && read_unlock_error)
		return read_unlock_error;
	return rc;
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	int use_default = !strcmp(argv[1], "default");
	CHECK(use_default || !strcmp(argv[1], "instance"));
	const char *mode = argv[2];
	char path[] = "/tmp/logger-control-error-XXXXXX";
	enter_temp(path);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = 0;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	LOGGER_INFO(owner, "control", "before-error");

	int is_reopen = strstr(mode, "reopen") != NULL;
	int expected = 0;
	if (!strcmp(mode, "setup")) {
		expected = EAGAIN;
		setup_error = expected;
	} else if (!strcmp(mode, "restore")) {
		expected = EIO;
		restore_error = expected;
	} else if (!strcmp(mode, "sync") || !strcmp(mode, "primary")) {
		expected = ENOSPC;
		sync_error = expected;
		if (!strcmp(mode, "primary"))
			restore_error = EIO;
	} else if (!strcmp(mode, "reopen") || !strcmp(mode, "reopen-primary")) {
		expected = EACCES;
		reopen_error = expected;
		if (!strcmp(mode, "reopen-primary"))
			restore_error = EIO;
	} else if (!strcmp(mode, "reopen-restore")) {
		expected = EIO;
		restore_error = expected;
	} else if (!strcmp(mode, "lock")) {
		expected = EAGAIN;
		emit_lock_error = expected;
	} else if (!strcmp(mode, "unlock")) {
		expected = EIO;
		emit_unlock_error = expected;
	} else if (!strcmp(mode, "read-lock")) {
		CHECK(use_default);
		expected = EAGAIN;
		read_error = expected;
	} else if (!strcmp(mode, "read-unlock")) {
		CHECK(use_default);
		expected = EPROTO;
		read_unlock_error = expected;
	} else {
		CHECK(!strcmp(mode, "healthy"));
	}

	active = 1;
	errno = EDOM;
	int rc = is_reopen ? (use_default ? logger_default_reopen() :
					    logger_reopen(owner)) :
			     (use_default ? logger_default_flush() :
					    logger_flush(owner));
	int saved = errno;
	active = 0;
	CHECK(rc == -expected && saved == EDOM);
	if (setup_error || read_error || emit_lock_error)
		CHECK(sync_calls == 0 && reopen_calls == 0);
	else if (is_reopen)
		CHECK(reopen_calls == 1);
	else
		CHECK(sync_calls > 0);

	if (read_unlock_error || emit_unlock_error) {
		/*
		 * 同步证明失败不能通过测试强制重置后再清理；进程退出归还资源。
		 * 这不是普通健康销毁证据，也不声称未知解锁状态可继续使用。
		 */
		CHECK(atomic_load(&owner->default_borrowed) == 1);
	} else {
		CHECK(!logger_scope_busy());
		CHECK(logger_default_set(owner, NULL) == 0);
		int close_rc = logger_destroy(&owner);
		CHECK(close_rc == -(sync_error ? sync_error : reopen_error));
		CHECK(!owner && logger_process_object_count() == 0);
	}
	leave_temp(path);
	printf("CONTROL_ERROR_PASS=%s/%s result=%d errno=%d\n", argv[1], mode,
	       rc, saved);
	return 0;
}
