#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "logger_libc.h"
#include "logger_lockdep.h"
#include "logger_fork_fd.h"
#include "logger_file.h"
#include "logger_syslog.h"
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

/* 对象身份先固定，再注入；绝不依赖“第一个 mutex 就是目标锁”。 */
static pthread_mutex_t *format_mutex;
static _Atomic int reject_lock;
static _Atomic int reject_unlock;
static _Atomic int reject_format;
static _Atomic int reject_archive_format;
static _Atomic int reject_localtime;
static _Atomic unsigned int format_locks;
static _Atomic unsigned int format_unlocks;
static _Atomic int fail_line_once;
static _Atomic int poison_after_lock;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __real_vsnprintf(char *out, size_t cap, const char *fmt, va_list args);
struct tm *__real_localtime_r(const time_t *seconds, struct tm *out);

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
	int archive = strstr(fmt, "%06ldZ") != NULL;

	if (archive && atomic_exchange(&reject_archive_format, 0)) {
		errno = EILSEQ;
		return -1;
	}
	if (reject_format) {
		reject_format = 0;
		errno = EILSEQ;
		return -1;
	}
	return __real_vsnprintf(out, cap, fmt, args);
}

/* 返回值仍来自真实 libc；仅此反例注入一次确定性 EILSEQ。 */
struct tm *__wrap_localtime_r(const time_t *seconds, struct tm *out)
{
	if (atomic_exchange(&reject_localtime, 0)) {
		errno = EILSEQ;
		return NULL;
	}
	return __real_localtime_r(seconds, out);
}

size_t __real_logger_format_line(const logger_t *logger,
				 const struct logger_message *message,
				 char *out, size_t cap);

size_t __wrap_logger_format_line(const logger_t *logger,
				 const struct logger_message *message,
				 char *out, size_t cap)
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
	logger_t *logger = NULL;

	CHECK(logger_create(&config, &logger) == 0);
	CHECK(logger != NULL);
	atomic_store(&fail_line_once, 1);
	errno = EDOM;
	CHECK(LOGGER_INFO(logger, "coordination", "NOT_A_SUCCESSFUL_RECORD") ==
	      (asynchronous ? 0 : -EIO));
	CHECK(errno == EDOM);
	CHECK(logger_flush(logger) == -EIO);
	logger_io_metrics_t metrics;

	CHECK(logger_get_io_metrics(logger, &metrics) == 0);
	CHECK(metrics.emitted_records == 0 && metrics.failed_records == 1);
	CHECK(metrics.async_completed + metrics.sync_completed == 1);
	CHECK(metrics.first_error == EIO && file_size("out.log") == 0);
	CHECK(!atomic_load(&fail_line_once));
	CHECK(logger_destroy(&logger) == -EIO);
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

/*
 * 两个后端只有在 snprintf 成功且确实溢出时才能报告容量错误。
 * libc 封装因锁或格式失败返回 -1 时，其真实 errno 必须穿过后端。
 */
static void syslog_prefix_failure(int lock_failure)
{
	char directory[] = "/tmp/logger-prefix-XXXXXX";

	enter_temp(directory);
	int type = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	int sink = socket(AF_UNIX, type, 0);

	CHECK(sink >= 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	const char name[] = "sink.sock";

	memcpy(address.sun_path, name, sizeof(name));
	size_t length = offsetof(struct sockaddr_un, sun_path) + sizeof(name);

	CHECK(bind(sink, (struct sockaddr *)&address, (socklen_t)length) == 0);

	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.syslog.path = name;
	struct logger_syslog backend;

	CHECK(logger_syslog_init(&backend, "unit", &cfg.syslog) == 0);

	if (lock_failure)
		atomic_store(&reject_lock, 1);
	else
		atomic_store(&reject_format, 1);
	int expected = lock_failure ? EAGAIN : EILSEQ;

	CHECK(logger_syslog_write(&backend, LOGGER_INFO, "reject\n", 7) == -1);
	CHECK(errno == expected);
	CHECK(!atomic_load(&reject_lock) && !atomic_load(&reject_format));
	CHECK(backend.metrics.submitted_records == 1 &&
	      backend.metrics.failed_records == 1 &&
	      backend.metrics.sent_records == 0 &&
	      backend.metrics.last_error == expected &&
	      backend.metrics.send_attempts == 0);
	CHECK(backend.metrics.backpressure_records ==
	      (unsigned int)lock_failure);
	char packet[512];

	CHECK(recv(sink, packet, sizeof(packet) - 1, MSG_DONTWAIT) == -1);
	CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

	/* 失败没有提交半条报文；后续调用可以独立成功。 */
	CHECK(logger_syslog_write(&backend, LOGGER_INFO, "recover\n", 8) == 0);
	ssize_t n = recv(sink, packet, sizeof(packet) - 1, MSG_DONTWAIT);

	CHECK(n > 0 && (size_t)n < sizeof(packet));
	packet[n] = 0;
	CHECK(strstr(packet, "recover") != NULL);
	CHECK(backend.metrics.sent_records == 1 &&
	      backend.metrics.failed_records == 1 &&
	      backend.metrics.last_error == 0);
	CHECK(logger_syslog_close_status(&backend) == 0);
	CHECK(close(sink) == 0);
	CHECK(unlink(name) == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(directory) == 0);
}

static void archive_format_failure(void)
{
	char directory[] = "/tmp/logger-archive-format-XXXXXX";

	enter_temp(directory);
	logger_rotation_config_t rotation = { 0 };

	rotation.mode = LOGGER_ROTATE_SIZE;
	rotation.max_file_size = 1;
	struct logger_file backend = LOGGER_FILE_EMPTY;

	CHECK(logger_file_init(&backend, "out.log", rotation, 0600) == 0);
	struct timespec stamp = { 1700000000, 0 };

	CHECK(logger_file_write(&backend, "a", 1, &stamp, 1) == 0);
	atomic_store(&reject_archive_format, 1);
	CHECK(logger_file_write(&backend, "b", 1, &stamp, 1) == -EILSEQ);
	CHECK(!atomic_load(&reject_archive_format));
	CHECK(backend.metrics.rotation_attempts == 1);
	CHECK(backend.metrics.rotation_failures == 1);
	CHECK(backend.metrics.last_rotation_error == EILSEQ);
	CHECK(backend.metrics.last_write_error == EILSEQ);
	CHECK(!backend.detached && backend.current_size == 1);
	CHECK(file_size("out.log") == 1);
	CHECK(file_contains("out.log", "a"));

	/* 归档命名失败没有改名，也没有破坏仍可用的旧 fd。 */
	CHECK(logger_file_write(&backend, "c", 1, &stamp, 1) == 0);
	CHECK(backend.metrics.rotation_successes == 1);
	CHECK(file_size("out.log") == 1);
	CHECK(file_contains("out.log", "c"));
	CHECK(logger_file_close_status(&backend) == 0);

	DIR *stream = opendir(".");

	CHECK(stream);
	struct dirent *entry;

	while ((entry = readdir(stream))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		CHECK(unlink(entry->d_name) == 0);
	}
	CHECK(!closedir(stream));
	CHECK(!chdir("/"));
	CHECK(!rmdir(directory));
}

/*
 * 进程域的格式协调失败不能伪装成时间值超界。
 * 首次构造和按日期写入使用同一生产函数，均需能恢复。
 */
static void calendar_error(int during_init, int lock_failure)
{
	char directory[] = "/tmp/logger-calendar-XXXXXX";

	enter_temp(directory);
	logger_rotation_config_t rotation = { 0 };

	rotation.mode = LOGGER_ROTATE_DAILY;
	struct logger_file backend = LOGGER_FILE_EMPTY;
	int expected = lock_failure ? EAGAIN : EILSEQ;
	struct timespec stamp = { .tv_sec = time(NULL), .tv_nsec = 0 };

	if (during_init) {
		if (lock_failure)
			atomic_store(&reject_lock, 1);
		else
			atomic_store(&reject_localtime, 1);
		errno = EDOM;
		CHECK(logger_file_init(&backend, "out.log", rotation, 0600) ==
		      -1);
		CHECK(errno == expected);
		CHECK(!logger_fd_valid(&backend.fd));
		CHECK(!logger_fd_valid(&backend.dir_fd));
		CHECK(!logger_fd_valid(&backend.lock_fd));
	}
	CHECK(logger_file_init(&backend, "out.log", rotation, 0600) == 0);

	if (!during_init) {
		if (lock_failure)
			atomic_store(&reject_lock, 1);
		else
			atomic_store(&reject_localtime, 1);
		CHECK(logger_file_write(&backend, "reject", 6, &stamp, 1) ==
		      -expected);
		CHECK(backend.metrics.failed_write_operations == 1);
		CHECK(backend.metrics.last_write_error == expected);
		CHECK(!backend.detached && backend.current_size == 0);
		CHECK(file_size("out.log") == 0);
	}
	CHECK(!atomic_load(&reject_lock) && !atomic_load(&reject_localtime));
	CHECK(logger_file_write(&backend, "recover", 7, &stamp, 1) == 0);
	CHECK(file_contains("out.log", "recover"));
	CHECK(logger_file_close_status(&backend) == 0);
	CHECK(unlink("out.log") == 0);
	CHECK(unlink("out.log.logger.lock") == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(directory) == 0);
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
	logger_t *logger = NULL;

	CHECK(logger_create(&config, &logger) == 0);
	CHECK(logger != NULL);
	format_locks = 0;
	format_unlocks = 0;
	reject_lock = !unlock;
	reject_unlock = unlock;
	int error = unlock ? EIO : EAGAIN;

	errno = EDOM;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "coordination",
					NULL, 0, NULL, "NOT_ACCEPTED");
	CHECK(rc == -error && errno == EDOM);
	CHECK(file_size("out.log") == 0);
	CHECK(format_locks == 1 && format_unlocks == (unlock ? 1u : 0u));
	if (unlock) {
		/* 域已毒化，不把无法证明释放的对象当作可重试指针。 */
		CHECK(logger_destroy(&logger) == -EIO);
	} else {
		CHECK(logger_log_sync_status(logger, LOGGER_INFO,
					     "coordination", NULL, 0, NULL,
					     "AFTER_RETRY") == 0);
		CHECK(logger_destroy(&logger) == 0);
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
		int (*format)(char *out, size_t cap, const char *fmt, ...) =
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
		   !strcmp(argv[1], "dual")) {
		exercise_failure(argv[1]);
	} else if (!strcmp(argv[1], "syslog-prefix-lock")) {
		syslog_prefix_failure(1);
	} else if (!strcmp(argv[1], "syslog-prefix-format")) {
		syslog_prefix_failure(0);
	} else if (!strcmp(argv[1], "archive-format")) {
		archive_format_failure();
	} else if (!strcmp(argv[1], "calendar-init-lock")) {
		calendar_error(1, 1);
	} else if (!strcmp(argv[1], "calendar-init-libc")) {
		calendar_error(1, 0);
	} else if (!strcmp(argv[1], "calendar-write-lock")) {
		calendar_error(0, 1);
	} else if (!strcmp(argv[1], "calendar-write-libc")) {
		calendar_error(0, 0);
	} else if (!strcmp(argv[1], "checked-lock")) {
		checked_log(0);
	} else if (!strcmp(argv[1], "checked-unlock")) {
		checked_log(1);
	} else if (!strcmp(argv[1], "output-sync")) {
		output_error(0);
	} else if (!strcmp(argv[1], "output-async")) {
		output_error(1);
	} else if (!strcmp(argv[1], "order-ok")) {
		lock_order(0, 0);
	} else if (!strcmp(argv[1], "order-reversed")) {
		lock_order(1, 0);
	} else if (!strcmp(argv[1], "order-recursive")) {
		lock_order(0, 1);
	} else {
		CHECK(0);
	}
	printf("LIBC_COORDINATION_PASS=%s\n", argv[1]);
	return 0;
}
