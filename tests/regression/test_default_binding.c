#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include <stdint.h>
#include <stdarg.h>

static logger_t *a;
static logger_t *b;
static _Atomic int reached, resume_call, setter_waiting, setter_done;
static _Atomic int cancel_cleaned;
static _Thread_local int block_read, block_control, block_write;
static _Thread_local int control_error, writer_error, read_error;
static _Thread_local int rw_unlock_error, mu_unlock_error, restore_error;
static _Thread_local int reenter;
static int read_rc;
static int read_errno;
static int setter_rc;
static int setter_errno;
static int cancel_setter;
static uintptr_t tracked;
static _Atomic int frees;

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *);
int __real_pthread_setcancelstate(int, int *);
int __real_logger_runtime_free(void *);
int __real_logger_vlog_internal(logger_t *, logger_level_t, const char *,
				const char *, int, const char *, const char *,
				va_list);

static void pause_call(void)
{
	atomic_store(&reached, 1);
	wait_flag(&resume_call);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (mu == &logger_runtime_current()->global_control_mu) {
		if (block_control) {
			block_control = 0;
			pause_call();
		}
		if (control_error) {
			control_error = 0;
			return EAGAIN;
		}
	}
	return __real_pthread_mutex_lock(mu);
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (block_read) {
		block_read = 0;
		pause_call();
	}
	if (read_error) {
		read_error = 0;
		return EAGAIN;
	}
	return __real_pthread_rwlock_rdlock(rw);
}

int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	atomic_store(&setter_waiting, 1);
	if (writer_error) {
		writer_error = 0;
		return EAGAIN;
	}
	return __real_pthread_rwlock_wrlock(rw);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
	int rc = __real_pthread_rwlock_unlock(rw);
	if (!rc && rw_unlock_error) {
		rw_unlock_error = 0;
		return EIO;
	}
	return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	int rc = __real_pthread_mutex_unlock(mu);
	if (!rc && mu_unlock_error &&
	    mu == &logger_runtime_current()->global_control_mu) {
		mu_unlock_error = 0;
		return EIO;
	}
	return rc;
}

int __wrap_pthread_setcancelstate(int state, int *old)
{
	int rc = __real_pthread_setcancelstate(state, old);
	if (!rc && state == PTHREAD_CANCEL_ENABLE && restore_error) {
		restore_error = 0;
		return EIO;
	}
	return rc;
}

int __wrap_logger_runtime_free(void *p)
{
	uintptr_t address = (uintptr_t)p;
	int rc = __real_logger_runtime_free(p);
	if (!rc && address == tracked)
		atomic_fetch_add(&frees, 1);
	return rc;
}

int __wrap_logger_vlog_internal(logger_t *l, logger_level_t level,
				const char *module, const char *file, int line,
				const char *func, const char *fmt, va_list ap)
{
	if (block_write) {
		block_write = 0;
		pause_call();
	}
	if (reenter) {
		reenter = 0;
		errno = ERANGE;
		CHECK(logger_default_set(a, NULL) == -EDEADLK);
		CHECK(errno == ERANGE);
	}
	return __real_logger_vlog_internal(l, level, module, file, line, func,
					   fmt, ap);
}

static logger_t *create(const char *path)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = path;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = 0;
	logger_t *l = NULL;
	CHECK(logger_create(&cfg, &l) == 0);
	return l;
}

static void *reader(void *arg)
{
	if (arg)
		block_read = 1;
	else
		block_write = 1;
	errno = EDOM;
	read_rc = LOG_INFO("DELAYED_A");
	read_errno = errno;
	return NULL;
}

static void cancel_cleanup(void *unused)
{
	(void)unused;
	/* 不能根据 set 是否返回猜测提交状态；按预期对象解绑再处置 owner。 */
	CHECK(logger_default_set(b, NULL) == 0);
	CHECK(logger_destroy(&b) == 0 && !b);
	atomic_store(&cancel_cleaned, 1);
}

static void *setter(void *unused)
{
	(void)unused;
	pthread_cleanup_push(cancel_cleanup, NULL);
	errno = EDOM;
	setter_rc = logger_default_set(a, b);
	setter_errno = errno;
	atomic_store(&setter_done, 1);
	if (cancel_setter)
		pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}

static void *stale_setter(void *unused)
{
	(void)unused;
	block_control = 1;
	setter_rc = logger_default_set(a, NULL);
	return NULL;
}

static void race_case(const char *mode)
{
	pthread_t thread, control;
	int delayed = !strcmp(mode, "stale-reader");
	int stale = !strcmp(mode, "stale-control") || !strcmp(mode, "aba");
	atomic_store(&setter_waiting, 0);
	cancel_setter = !strcmp(mode, "cancel");
	CHECK(pthread_create(&thread, NULL, stale ? stale_setter : reader,
			     delayed ? (void *)1 : NULL) == 0);
	wait_flag(&reached);
	if (delayed || stale) {
		CHECK(logger_default_set(a, b) == 0);
		if (!strcmp(mode, "aba"))
			CHECK(logger_default_set(b, a) == 0);
	} else {
		CHECK(pthread_create(&control, NULL, setter, NULL) == 0);
		wait_flag(&setter_waiting);
		CHECK(!atomic_load(&setter_done));
		CHECK(logger_destroy(&a) == -EBUSY && a);
		int touched = 42;
		errno = EDOM;
		CHECK(LOG_INFO("GATE%n", &touched) == -EAGAIN);
		CHECK(errno == EDOM && touched == 42);
		if (cancel_setter)
			CHECK(pthread_cancel(control) == 0);
	}
	atomic_store(&resume_call, 1);
	CHECK(pthread_join(thread, NULL) == 0);
	if (stale)
		CHECK(setter_rc == -ESTALE);
	else if (delayed)
		CHECK(read_rc == -ESHUTDOWN && read_errno == EDOM);
	else {
		void *result;
		CHECK(pthread_join(control, &result) == 0);
		CHECK(setter_rc == 0 && setter_errno == EDOM);
		CHECK(read_rc == 0 && read_errno == EDOM);
		if (cancel_setter)
			CHECK(result == PTHREAD_CANCELED &&
			      atomic_load(&cancel_cleaned) && !b);
	}
	if (!strcmp(mode, "aba")) {
		CHECK(logger_default_set(a, NULL) == 0);
	} else if (b) {
		CHECK(logger_default_set(b, NULL) == 0);
	}
	CHECK(!atomic_load(&a->default_borrowed));
	CHECK(atomic_load(&frees) == 0);
}

static void *failing_setter(void *arg)
{
	const char *mode = arg;
	if (!strcmp(mode, "lifetime-unlock"))
		rw_unlock_error = 1;
	else
		mu_unlock_error = 1;
	errno = ERANGE;
	setter_rc = logger_default_set(a, b);
	setter_errno = errno;
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	(void)alarm(20);
	char path[] = "/tmp/logger-default-regression-XXXXXX";
	enter_temp(path);
	CHECK(logger_process_init() == 0);
	a = create("a.log");
	b = create("b.log");
	tracked = (uintptr_t)a;
	CHECK(logger_default_set(NULL, a) == 0);
	const char *mode = argv[1];
	if (!strcmp(mode, "held-reader") || !strcmp(mode, "stale-reader") ||
	    !strcmp(mode, "stale-control") || !strcmp(mode, "aba") ||
	    !strcmp(mode, "cancel")) {
		race_case(mode);
	} else if (!strcmp(mode, "lifetime-unlock") ||
		   !strcmp(mode, "control-unlock")) {
		pthread_t thread;
		CHECK(pthread_create(&thread, NULL, failing_setter, argv[1]) ==
		      0);
		CHECK(pthread_join(thread, NULL) == 0);
		CHECK(setter_rc == -EIO && setter_errno == ERANGE);
		CHECK(atomic_load(&a->default_borrowed));
		CHECK(atomic_load(&b->default_borrowed));
		CHECK(logger_destroy(&a) == -EBUSY && a);
		CHECK(logger_destroy(&b) == -EBUSY && b);
		CHECK(logger_default_set(b, NULL) == -EIO);
		CHECK(!atomic_load(&frees));
		/* 回执不确定时按合同保留，独立进程结束后由内核回收。 */
		CHECK(unlink("a.log") == 0 && unlink("b.log") == 0);
		CHECK(unlink("a.log.logger.lock") == 0);
		CHECK(unlink("b.log.logger.lock") == 0);
		CHECK(chdir("/") == 0 && rmdir(path) == 0);
		printf("DEFAULT_BORROW_PASS=%s retained=2\n", mode);
		return 0;
	} else {
		if (!strcmp(mode, "control-error"))
			control_error = 1;
		else if (!strcmp(mode, "writer-error"))
			writer_error = 1;
		else if (!strcmp(mode, "read-error")) {
			read_error = 1;
			errno = EDOM;
			CHECK(LOG_INFO("NOT_WRITTEN") == -EAGAIN);
			CHECK(errno == EDOM);
		} else if (!strcmp(mode, "reentry")) {
			reenter = 1;
			LOG_INFO("REENTRY_CHECKED");
			CHECK(!reenter);
		} else if (!strcmp(mode, "restore-error")) {
			restore_error = 1;
		} else if (!strcmp(mode, "stopped")) {
			atomic_store(&b->state, LOGGER_STATE_STOPPING);
			CHECK(logger_default_set(a, b) == -ESHUTDOWN);
			atomic_store(&b->state, LOGGER_STATE_RUNNING);
		} else if (!strcmp(mode, "overflow")) {
			uint64_t old = atomic_load(
				&logger_runtime_current()->global_ticket);
			atomic_store(&logger_runtime_current()->global_ticket,
				     (UINT64_MAX & ~UINT64_C(7)) | (old & 7));
			CHECK(logger_default_set(a, b) == -EOVERFLOW);
			atomic_store(&logger_runtime_current()->global_ticket,
				     old);
		} else {
			CHECK(0);
		}
		errno = EDOM;
		int rc = logger_default_set(a, b);
		if (strstr(mode, "-error") && (!strcmp(mode, "control-error") ||
					       !strcmp(mode, "writer-error"))) {
			CHECK(rc == -EAGAIN && errno == EDOM);
			CHECK(atomic_load(&a->default_borrowed));
			CHECK(!atomic_load(&b->default_borrowed));
			CHECK(logger_default_set(a, b) == 0);
		} else if (!strcmp(mode, "restore-error")) {
			CHECK(rc == -EIO && errno == EDOM);
		} else {
			CHECK(rc == 0 && errno == EDOM);
		}
		CHECK(logger_default_set(b, NULL) == 0);
	}
	CHECK(logger_destroy(&a) == 0 && !a);
	CHECK(atomic_load(&frees) == 1);
	CHECK(logger_destroy(&b) == 0 && !b);
	CHECK(logger_process_object_count() == 0);
	CHECK(logger_runtime_current()->allocation_count == 0);
	const char *files[] = { "a.log", "b.log", "a.log.logger.lock",
				"b.log.logger.lock" };
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
		CHECK(unlink(files[i]) == 0);
	CHECK(chdir("/") == 0 && rmdir(path) == 0);
	printf("DEFAULT_BORROW_PASS=%s\n", mode);
	return 0;
}
