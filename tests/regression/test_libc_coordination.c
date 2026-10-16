#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "logger_libc.h"
#include "logger_lockdep.h"
#include "logger_fork_fd.h"
#include <signal.h>
#include <sys/wait.h>

/* 对象身份先固定，再注入；绝不依赖“第一个 mutex 就是目标锁”。 */
static pthread_mutex_t *format_mutex;
static _Atomic int reject_lock;
static _Atomic int reject_unlock;
static _Atomic int reject_format;
static _Atomic unsigned format_locks;
static _Atomic unsigned format_unlocks;
static _Atomic int fail_line_once;
static _Atomic int poison_after_lock;

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_vsnprintf(char *, size_t, const char *, va_list);

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (mutex == format_mutex) {
		++format_locks;
		if (reject_lock) {
			reject_lock = 0;
			return EAGAIN;
		}
	}
	int rc = __real_pthread_mutex_lock(mutex);
	if (!rc && mutex == format_mutex &&
	    atomic_exchange(&poison_after_lock, 0))
		logger_fork_resource_poison(EIO);
	return rc;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_unlock(mutex);
	if (!rc && mutex == format_mutex) {
		++format_unlocks;
		if (reject_unlock) {
			reject_unlock = 0;
			return EIO;
		}
	}
	return rc;
}

int __wrap_vsnprintf(char *out, size_t cap, const char *fmt, va_list args)
{
	if (reject_format) {
		reject_format = 0;
		errno = EILSEQ;
		return -1;
	}
	return __real_vsnprintf(out, cap, fmt, args);
}

size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
size_t __wrap_logger_format_line(const logger_t *logger,
				 const logger_message_t *message, char *out,
				 size_t cap)
{
	if (atomic_exchange(&fail_line_once, 0))
		atomic_store(&reject_unlock, 1);
	return __real_logger_format_line(logger, message, out, cap);
}

static void output_error(int asynchronous)
{
	char directory[] = "/tmp/logger-libc-output-XXXXXX";
	enter_temp(directory);
	logger_config_t config = LOGGER_DEFAULT_CONFIG();
	config.async_mode = asynchronous;
	config.queue_capacity = 8;
	config.outputs = LOGGER_OUT_FILE;
	config.file_path = "out.log";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&config);
	CHECK(logger);
	atomic_store(&fail_line_once, 1);
	LOGGER_INFO(logger, "coordination", "NOT_A_SUCCESSFUL_RECORD");
	CHECK(logger_flush_instance_status(logger) == -1 && errno == EIO);
	logger_io_metrics_t metrics;
	logger_get_io_metrics(logger, &metrics);
	CHECK(metrics.emitted_records == 0 && metrics.failed_records == 1);
	CHECK(metrics.async_completed + metrics.sync_completed == 1);
	CHECK(metrics.first_error == EIO && file_size("out.log") == 0);
	CHECK(!atomic_load(&fail_line_once));
	CHECK(logger_destroy_status(logger) == -1 && errno == EIO);
	leave_temp(directory);
}

static void exercise_failure(const char *mode)
{
	char out[32] = "UNCHANGED";
	int lock_error = !strcmp(mode, "lock");
	int dual_error = !strcmp(mode, "dual");
	reject_lock = lock_error;
	reject_unlock = !lock_error;
	reject_format = dual_error;
	errno = EDOM;
	int rc = logger_libc_snprintf(out, sizeof(out), "%s", "complete");
	int expected = lock_error ? EAGAIN : dual_error ? EILSEQ : EIO;
	CHECK(rc == -1 && errno == expected);
	CHECK(format_locks == 1 && format_unlocks == (lock_error ? 0u : 1u));
	CHECK(!reject_lock && !reject_unlock && !reject_format);
	if (lock_error) {
		CHECK(!strcmp(out, "UNCHANGED"));
		CHECK(!atomic_load(&logger_runtime_current()->resource_error));
		errno = EDOM;
		CHECK(logger_libc_snprintf(out, sizeof(out), "%s",
					   "complete") == 8);
		CHECK(!strcmp(out, "complete") && errno == EDOM);
		CHECK(format_locks == 2 && format_unlocks == 1);
	} else {
		CHECK(atomic_load(&logger_runtime_current()->resource_error) ==
		      EIO);
		CHECK(logger_libc_snprintf(out, sizeof(out), "%s",
					   "forbidden") == -1);
		CHECK(errno == EIO && format_locks == 1 && format_unlocks == 1);
	}
}

static void checked_log(int unlock)
{
	char directory[] = "/tmp/logger-libc-check-XXXXXX";
	enter_temp(directory);
	logger_config_t config = LOGGER_DEFAULT_CONFIG();
	config.async_mode = 0;
	config.outputs = LOGGER_OUT_FILE;
	config.file_path = "out.log";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *logger = logger_create(&config);
	CHECK(logger);
	format_locks = 0;
	format_unlocks = 0;
	reject_lock = !unlock;
	reject_unlock = unlock;
	int error = unlock ? EIO : EAGAIN;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "coordination",
					NULL, 0, NULL, "NOT_ACCEPTED");
	CHECK(rc == -error && errno == error);
	CHECK(file_size("out.log") == 0);
	CHECK(format_locks == 1 && format_unlocks == (unlock ? 1u : 0u));
	if (unlock) {
		/* 域已毒化，不把无法证明释放的对象当作可重试指针。 */
		CHECK(logger_destroy_status(logger) == -1 && errno == EIO);
	} else {
		CHECK(logger_log_sync_status(logger, LOGGER_INFO,
					     "coordination", NULL, 0, NULL,
					     "AFTER_RETRY") == 0);
		CHECK(logger_destroy_status(logger) == 0);
		CHECK(file_contains("out.log", "AFTER_RETRY"));
	}
	leave_temp(directory);
}

static void lock_order(int reversed, int recursive)
{
	if (!reversed && !recursive) {
		CHECK(logger_libc_fork_prepare() == 0);
		CHECK(logger_fork_resource_enter() == 0);
		CHECK(logger_fork_resource_leave(0) == 0);
		logger_libc_fork_parent();
		return;
	}
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		(void)alarm(5);
		if (logger_process_init())
			_exit(30);
		format_mutex = &logger_runtime_current()->format_mu;
		if (logger_fork_resource_enter())
			_exit(31);
		if (reversed) {
			char out[16];
			(void)logger_libc_snprintf(out, sizeof(out), "%s",
						   "bad-order");
		} else {
			/* 校验器必须在真实阻塞前识别同一地址重取。 */
			logger_lockdep_check_acquire(
				LOGGER_LOCK_RUNTIME_RESOURCE,
				&logger_runtime_current()->resource_mu);
		}
		_exit(32);
	}
	int status;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(logger_process_init() == 0);
	format_mutex = &logger_runtime_current()->format_mu;
	if (!strcmp(argv[1], "errno-format")) {
		char out[128];
		/* 函数指针不带 ISO printf 属性：这里专门验证 GNU %m。 */
		int (*format)(char *, size_t, const char *, ...) =
			logger_libc_snprintf;
		errno = EDOM;
		CHECK(format(out, sizeof(out), "%m") > 0);
		CHECK(errno == EDOM && !strcmp(out, strerror(EDOM)));
	} else if (!strcmp(argv[1], "post-lock-poison")) {
		char out[32] = "UNCHANGED";
		atomic_store(&poison_after_lock, 1);
		CHECK(logger_libc_snprintf(out, sizeof(out), "%s",
					   "forbidden") == -1);
		CHECK(errno == EIO && !strcmp(out, "UNCHANGED"));
		CHECK(format_locks == 1 && format_unlocks == 1);
		CHECK(!logger_libc_busy() && !atomic_load(&poison_after_lock));
	} else if (!strcmp(argv[1], "lock") || !strcmp(argv[1], "unlock") ||
		   !strcmp(argv[1], "dual"))
		exercise_failure(argv[1]);
	else if (!strcmp(argv[1], "checked-lock"))
		checked_log(0);
	else if (!strcmp(argv[1], "checked-unlock"))
		checked_log(1);
	else if (!strcmp(argv[1], "output-sync"))
		output_error(0);
	else if (!strcmp(argv[1], "output-async"))
		output_error(1);
	else if (!strcmp(argv[1], "order-ok"))
		lock_order(0, 0);
	else if (!strcmp(argv[1], "order-reversed"))
		lock_order(1, 0);
	else if (!strcmp(argv[1], "order-recursive"))
		lock_order(0, 1);
	else
		CHECK(0);
	printf("LIBC_COORDINATION_PASS=%s\n", argv[1]);
	return 0;
}
