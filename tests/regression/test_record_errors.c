#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include <stdarg.h>

/* 同一驱动分别链接真实生产静态归档和同源支持归档，不注入 DSO 内部。 */
enum record_api { RECORD_LOG, RECORD_SOURCE, RECORD_DEFAULT, RECORD_SYNC };

static logger_t *owner;
static enum record_api api;
static _Thread_local int active;
static int clock_error;
static int format_error;
static int write_error;
static int emit_lock_error;
static int emit_unlock_error;
static int format_lock_error;
static int format_unlock_error;
static int read_lock_error;
static int read_unlock_error;
static int notify_lock_error;
static int notify_signal_error;
static unsigned int fail_setup_number;
static unsigned int fail_restore_number;
static unsigned int setup_calls;
static unsigned int restore_calls;
static unsigned int clock_calls;
static unsigned int format_calls;
static unsigned int read_calls;
static unsigned int injected;
static int contaminate_errno;
static pthread_mutex_t *unowned_mutex;
static int defer_worker;
static int gate_wait_return;
static _Thread_local logger_t *worker_instance;
static _Atomic int worker_entered;
static _Atomic int release_worker;
static _Atomic int wait_entered;
static _Atomic int release_wait_return;
static _Atomic unsigned int wait_returns;
static void *(*worker_function)(void *arg);
static void *worker_argument;

int __real_clock_gettime(clockid_t clock, struct timespec *time);
int __real_vsnprintf(char *out, size_t cap, const char *fmt, va_list args);
int __real___vsnprintf_chk(char *out, size_t cap, int flag, size_t size,
			   const char *fmt, va_list args);
ssize_t __real_write(int fd, const void *data, size_t length);
int __real_pthread_setcancelstate(int state, int *old);
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *lock);
int __real_pthread_cond_signal(pthread_cond_t *cond);
int __real_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*function)(void *arg), void *argument);

static int take_error(int *error)
{
	int result = *error;
	if (result) {
		*error = 0;
		++injected;
	}
	return result;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *time)
{
	if (active && clock == CLOCK_REALTIME) {
		++clock_calls;
		int error = take_error(&clock_error);
		if (error) {
			errno = error;
			return -1;
		}
	}
	int rc = __real_clock_gettime(clock, time);
	if (!rc && active && contaminate_errno)
		errno = ERANGE;
	return rc;
}

static int reject_format(void)
{
	if (!active)
		return 0;
	++format_calls;
	int error = take_error(&format_error);
	if (error)
		errno = error;
	return error;
}

int __wrap_vsnprintf(char *out, size_t cap, const char *fmt, va_list args)
{
	if (reject_format())
		return -1;
	return __real_vsnprintf(out, cap, fmt, args);
}

/* 加固生产调用点必须实际命中，保留检查标志和对象大小，不关闭 FORTIFY。 */
int __wrap___vsnprintf_chk(char *out, size_t cap, int flag, size_t size,
			   const char *fmt, va_list args)
{
	if (reject_format())
		return -1;
	return __real___vsnprintf_chk(out, cap, flag, size, fmt, args);
}

ssize_t __wrap_write(int fd, const void *data, size_t length)
{
	if (active && owner &&
	    fd == logger_fd_number(logger_fd_borrow(&owner->file_backend.fd))) {
		int error = take_error(&write_error);
		if (error) {
			errno = error;
			return -1;
		}
	}
	return __real_write(fd, data, length);
}

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (active && old && ++setup_calls == fail_setup_number) {
		fail_setup_number = 0;
		++injected;
		return EAGAIN;
	}
	int rc = __real_pthread_setcancelstate(state, old);
	if (!rc && active && !old && ++restore_calls == fail_restore_number) {
		/* 先完成真实恢复，再报告错误；不能由测试自己破坏取消策略。 */
		fail_restore_number = 0;
		++injected;
		return EIO;
	}
	if (!rc && active && contaminate_errno)
		errno = ERANGE;
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	int error = 0;
	if (active) {
		if (mutex == &owner->emit_mu)
			error = take_error(&emit_lock_error);
		else if (mutex == &owner->q.wait_mu)
			error = take_error(&notify_lock_error);
		else if (mutex == &logger_runtime_current()->format_mu)
			error = take_error(&format_lock_error);
	}
	if (error) {
		unowned_mutex = mutex;
		return error;
	}
	int rc = __real_pthread_mutex_lock(mutex);
	if (!rc && active && unowned_mutex == mutex)
		unowned_mutex = NULL;
	return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	if (active)
		CHECK(mutex != unowned_mutex);
	int rc = __real_pthread_mutex_unlock(mutex);
	if (!rc && active) {
		int error = 0;
		if (mutex == &owner->emit_mu)
			error = take_error(&emit_unlock_error);
		else if (mutex == &logger_runtime_current()->format_mu)
			error = take_error(&format_unlock_error);
		if (error)
			return error;
	}
	return rc;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	if (active) {
		++read_calls;
		int error = take_error(&read_lock_error);
		if (error)
			return error;
	}
	int rc = __real_pthread_rwlock_rdlock(lock);
	if (!rc && active && contaminate_errno)
		errno = ERANGE;
	return rc;
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	int rc = __real_pthread_rwlock_unlock(lock);
	if (!rc && active) {
		int error = take_error(&read_unlock_error);
		if (error)
			return error;
	}
	return rc;
}

int __wrap_pthread_cond_signal(pthread_cond_t *cond)
{
	if (active && cond == &owner->q.wait_cv) {
		int error = take_error(&notify_signal_error);
		if (error)
			return error;
	}
	return __real_pthread_cond_signal(cond);
}

int __wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	if (!gate_wait_return || !worker_instance ||
	    cond != &worker_instance->q.wait_cv ||
	    mutex != &worker_instance->q.wait_mu)
		return __real_pthread_cond_wait(cond, mutex);

	atomic_store_explicit(&wait_entered, 1, memory_order_release);
	int rc = __real_pthread_cond_wait(cond, mutex);
	CHECK(rc == 0);
	/*
	 * 真实等待返回后暂时让出锁，但不能让工作线程在本次发布完成前
	 * disarm。这样伪唤醒也不能跳过待验证的通知链；回到生产代码时
	 * 重新持有同一把锁，保留 cond_wait 的返回合同。
	 */
	CHECK(__real_pthread_mutex_unlock(mutex) == 0);
	atomic_fetch_add_explicit(&wait_returns, 1, memory_order_release);
	wait_flag(&release_wait_return);
	CHECK(__real_pthread_mutex_lock(mutex) == 0);
	return rc;
}

static void *controlled_worker(void *unused)
{
	(void)unused;
	worker_instance = worker_argument;
	atomic_store_explicit(&worker_entered, 1, memory_order_release);
	if (defer_worker)
		wait_flag(&release_worker);
	return worker_function(worker_argument);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*function)(void *arg), void *argument)
{
	if (defer_worker || gate_wait_return) {
		worker_function = function;
		worker_argument = argument;
		return __real_pthread_create(thread, attr, controlled_worker,
					     NULL);
	}
	return __real_pthread_create(thread, attr, function, argument);
}

static int record(const char *fmt, const char *text)
{
	logger_source_t source = { .module = "record",
				   .file = "source.c",
				   .func = "error_source",
				   .line = 23 };

	switch (api) {
	case RECORD_LOG:
		return logger_log(owner, LOGGER_INFO, "record", __FILE__,
				  __LINE__, __func__, fmt, text);
	case RECORD_SOURCE:
		return logger_log_source(owner, LOGGER_INFO, source, fmt, text);
	case RECORD_DEFAULT:
		return logger_global_write(LOGGER_INFO, "record", __FILE__,
					   __LINE__, __func__, fmt, text);
	case RECORD_SYNC:
		return logger_log_sync_status(owner, LOGGER_INFO, "record",
					      __FILE__, __LINE__, __func__, fmt,
					      text);
	}
	CHECK(0);
	return -EINVAL;
}

static int call(const char *fmt, const char *text)
{
	active = 1;
	errno = EDOM;
	int rc = record(fmt, text);
	int saved = errno;
	active = 0;
	CHECK(saved == EDOM);
	return rc;
}

static void expect_line(FILE *file, const char *body)
{
	char line[8192];
	size_t length = strlen(body);
	CHECK(fgets(line, sizeof(line), file) != NULL);
	size_t total = strlen(line);
	CHECK(total > length + 1 && line[total - 1] == '\n');
	CHECK(!memcmp(line + total - length - 1, body, length));
}

/* 每次从头核对完整文件，不能由后继成功调用补足本次丢失或重复的记录。 */
static void expect_records(const char *body, size_t count, const char *tail)
{
	FILE *file = fopen("out.log", "rb");
	CHECK(file);
	for (size_t i = 0; i < count; ++i)
		expect_line(file, body);
	if (tail)
		expect_line(file, tail);
	CHECK(fgetc(file) == EOF && !ferror(file));
	CHECK(fclose(file) == 0);
}

static void check_cancel_state(void)
{
	int old;
	CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old) == 0);
	CHECK(old == PTHREAD_CANCEL_ENABLE);
	CHECK(pthread_setcancelstate(old, NULL) == 0);
}

static void cleanup(int error)
{
	CHECK(!logger_scope_busy());
	check_cancel_state();
	if (api == RECORD_DEFAULT)
		CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(logger_destroy(&owner) == -error);
	CHECK(owner == NULL && logger_process_object_count() == 0);
}

static void wait_completed(void)
{
	for (unsigned int i = 0; i < 5000; ++i) {
		if (atomic_load_explicit(&owner->async_completed,
					 memory_order_acquire) == 1)
			return;
		nap_ms();
	}
	CHECK(!"published record did not complete");
}

static void queue_case(const char *mode)
{
	int spill = !strcmp(mode, "spill-drop");
	int fallback = !strcmp(mode, "fallback-error");
	size_t accepted = spill ? owner->q.spill_cap : owner->q.cap;
	char long_body[600];
	memset(long_body, 's', sizeof(long_body) - 1);
	long_body[sizeof(long_body) - 1] = '\0';
	const char *body = spill ? long_body : "queued";

	wait_flag(&worker_entered);
	CHECK(accepted > 0 && accepted <= 1024);
	if (spill)
		CHECK(accepted < owner->q.cap);
	for (size_t i = 0; i < accepted; ++i)
		CHECK(call("%s", body) == 0);
	expect_records(body, 0, NULL);
	if (fallback)
		write_error = ENOSPC;
	CHECK(call("%s", body) == (fallback ? -ENOSPC : -EAGAIN));
	CHECK(!write_error && injected == (unsigned int)fallback);
	CHECK(atomic_load(&owner->enqueued) == accepted);
	CHECK(atomic_load(&owner->dropped_by_level[LOGGER_INFO]) ==
	      (unsigned int)!fallback);
	CHECK(atomic_load(&owner->sync_fallbacks) == (unsigned int)fallback);
	if (spill) {
		CHECK(atomic_load(&owner->q.spill_exhaustions) == 1);
		/* 溢出池拒绝后短记录仍能入队，证明拒绝来自池容量而非队列满。 */
		CHECK(call("%s", "short-after-spill") == 0);
		CHECK(atomic_load(&owner->enqueued) == accepted + 1);
	}
	expect_records(body, 0, NULL);
	atomic_store_explicit(&release_worker, 1, memory_order_release);
	CHECK(logger_flush(owner) == (fallback ? -ENOSPC : 0));
	expect_records(body, accepted, spill ? "short-after-spill" : NULL);
	CHECK(atomic_load(&owner->emitted_records) ==
	      accepted + (unsigned int)spill);
	CHECK(atomic_load(&owner->failed_records) == (unsigned int)fallback);
	cleanup(fallback ? ENOSPC : 0);
}

static void notify_case(const char *mode)
{
	int error = !strcmp(mode, "notify-lock") ? EAGAIN : EIO;
	wait_flag(&wait_entered);
	/*
	 * 取得真实 wait_mu 才证明工作线程已越过发布 waiting 后的复查，
	 * 并经 cond_wait 释放锁；仅轮询 waiting 无法证明这一交接。
	 */
	CHECK(__real_pthread_mutex_lock(&owner->q.wait_mu) == 0);
	CHECK(atomic_load(&owner->q.consumer_waiting));
	CHECK(logger_queue_empty(&owner->q));
	CHECK(__real_pthread_mutex_unlock(&owner->q.wait_mu) == 0);
	if (error == EAGAIN)
		notify_lock_error = error;
	else
		notify_signal_error = error;
	CHECK(call("%s", "published-once") == -error);
	CHECK(injected == 1 && !notify_lock_error && !notify_signal_error);
	CHECK(atomic_load(&owner->enqueued) == 1);
	CHECK(atomic_load(&owner->dropped_by_level[LOGGER_INFO]) == 0);
	CHECK(atomic_load(&owner->sync_fallbacks) == 0);
	CHECK(atomic_load(&owner->first_error) == error);
	CHECK(atomic_load(&owner->lifecycle_error) == error);
	CHECK(atomic_load(&owner->state) == LOGGER_STATE_STOPPING);
	atomic_store_explicit(&release_wait_return, 1, memory_order_release);
	wait_completed();
	CHECK(atomic_load_explicit(&wait_returns, memory_order_acquire) > 0);
	expect_records("published-once", 1, NULL);
	CHECK(call("%s", "must-not-be-retried") == -ESHUTDOWN);
	expect_records("published-once", 1, NULL);
	errno = EDOM;
	CHECK(logger_flush(owner) == -ESHUTDOWN && errno == EDOM);
	cleanup(error);
}

static int simple_case(const char *mode)
{
	int expected = 0;
	unsigned int expected_injections = 1;
	size_t records = 0;
	int retain = 0;
	int sticky = 0;
	unsigned int outer_restore = api == RECORD_DEFAULT ? 2 : 1;

	if (!strcmp(mode, "clock")) {
		expected = EFAULT;
		clock_error = expected;
	} else if (!strcmp(mode, "format") || !strcmp(mode, "format-primary")) {
		expected = EILSEQ;
		format_error = expected;
		if (!strcmp(mode, "format-primary")) {
			fail_restore_number = outer_restore;
			++expected_injections;
		}
	} else if (!strcmp(mode, "write") || !strcmp(mode, "write-primary")) {
		sticky = ENOSPC;
		expected = sticky;
		write_error = expected;
		if (!strcmp(mode, "write-primary")) {
			fail_restore_number = outer_restore;
			++expected_injections;
		}
	} else if (!strcmp(mode, "setup") || !strcmp(mode, "inner-setup")) {
		fail_setup_number = !strcmp(mode, "setup") ? 1 : 2;
		CHECK(fail_setup_number == 1 || api == RECORD_DEFAULT);
		expected = EAGAIN;
	} else if (!strcmp(mode, "restore") || !strcmp(mode, "inner-restore")) {
		fail_restore_number = !strcmp(mode, "restore") ? outer_restore :
								 1;
		CHECK(strcmp(mode, "inner-restore") || api == RECORD_DEFAULT);
		expected = EIO;
		records = 1;
	} else if (!strcmp(mode, "errno")) {
		contaminate_errno = 1;
		expected_injections = 0;
		records = 1;
	} else if (!strcmp(mode, "emit-lock")) {
		sticky = EBUSY;
		expected = sticky;
		emit_lock_error = expected;
	} else if (!strcmp(mode, "emit-unlock")) {
		expected = EPERM;
		emit_unlock_error = expected;
		records = 1;
		retain = 1;
	} else if (!strcmp(mode, "format-lock")) {
		expected = EBUSY;
		format_lock_error = expected;
	} else if (!strcmp(mode, "format-unlock")) {
		expected = EPERM;
		format_unlock_error = expected;
		retain = 1;
	} else if (!strcmp(mode, "read-lock")) {
		CHECK(api == RECORD_DEFAULT);
		expected = EAGAIN;
		read_lock_error = expected;
	} else {
		CHECK(api == RECORD_DEFAULT);
		CHECK(!strcmp(mode, "read-unlock") ||
		      !strcmp(mode, "read-unlock-primary"));
		expected = EPROTO;
		read_unlock_error = expected;
		retain = 1;
		records = 1;
		if (!strcmp(mode, "read-unlock-primary")) {
			write_error = ENOSPC;
			++expected_injections;
			records = 0;
		}
	}

	char expected_body[256];
	const char *body = "probe";
	if (contaminate_errno) {
		int n = snprintf(expected_body, sizeof(expected_body),
				 "probe errno=%s", strerror(EDOM));
		CHECK(n > 0 && (size_t)n < sizeof(expected_body));
		body = expected_body;
	}
	CHECK(call(contaminate_errno ? "%s errno=%m" : "%s", "probe") ==
	      -expected);
	CHECK(injected == expected_injections);
	CHECK(!clock_error && !format_error && !write_error &&
	      !emit_lock_error && !emit_unlock_error && !format_lock_error &&
	      !format_unlock_error && !read_lock_error && !read_unlock_error &&
	      !fail_setup_number && !fail_restore_number);
	if (!strcmp(mode, "setup") || !strcmp(mode, "inner-setup") ||
	    !strcmp(mode, "read-lock"))
		CHECK(clock_calls == 0 && format_calls == 0);
	if (!strcmp(mode, "clock") || !strcmp(mode, "format-lock"))
		CHECK(format_calls == 0);
	if (contaminate_errno) {
		CHECK(clock_calls == 1 && format_calls > 0 && setup_calls > 0);
		CHECK(read_calls == (unsigned int)(api == RECORD_DEFAULT));
	}
	expect_records(body, records, NULL);
	if (retain) {
		/*
		 * 已报告的解锁证明失败按合同保留，不能强制重置后伪造健康清理。
		 */
		if (!strcmp(mode, "emit-unlock"))
			CHECK(atomic_load(&owner->synchronization_error) ==
			      EPERM);
		else if (!strcmp(mode, "format-unlock"))
			CHECK(atomic_load(&logger_runtime_current()
						   ->resource_error) == EPERM);
		else
			CHECK(atomic_load(&owner->default_borrowed) == 1);
		return 1;
	}
	check_cancel_state();
	contaminate_errno = 0;
	CHECK(call("%s", "after-failure") == 0);
	expect_records(body, records, "after-failure");
	cleanup(sticky);
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	const char *names[] = { "log", "source", "default", "sync" };
	unsigned int index;
	for (index = 0; index < 4 && strcmp(argv[1], names[index]); ++index) {
	}
	CHECK(index < 4);
	api = (enum record_api)index;
	const char *mode = argv[2];
	int queue = !strcmp(mode, "queue-drop") ||
		    !strcmp(mode, "spill-drop") ||
		    !strcmp(mode, "fallback-error");
	int notify = !strcmp(mode, "notify-lock") ||
		     !strcmp(mode, "notify-signal");
	CHECK(!(queue || notify) || api != RECORD_SYNC);
	defer_worker = queue;
	gate_wait_return = notify;
	int retained = 0;
	char directory[] = "/tmp/logger-record-errors-XXXXXX";
	enter_temp(directory);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.detail = LOGGER_DETAIL_MINIMAL;
	cfg.flush_level = LOGGER_OFF;
	cfg.async_mode = queue || notify;
	cfg.queue_capacity = !strcmp(mode, "spill-drop") ? 2048 : 8;
	cfg.overflow[LOGGER_INFO] = !strcmp(mode, "fallback-error") ?
					    LOGGER_OVERFLOW_SYNC :
					    LOGGER_OVERFLOW_DROP;
	CHECK(logger_create(&cfg, &owner) == 0);
	if (api == RECORD_DEFAULT)
		CHECK(logger_default_set(NULL, owner) == 0);
	if (queue)
		queue_case(mode);
	else if (notify)
		notify_case(mode);
	else
		retained = simple_case(mode);
	/*
	 * 保守保留的实例仍拥有文件和协调锁，夹具随进程退出保留，不能
	 * 调用健康销毁后的清理辅助函数删除存活锁 inode。
	 */
	if (retained)
		CHECK(chdir("/") == 0);
	else
		leave_temp(directory);
	printf("RECORD_ERROR_PASS=%s/%s injections=%u\n", argv[1], mode,
	       injected);
	return 0;
}
