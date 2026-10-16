#include "logger_libc.h"
#include "logger_runtime.h"
#include "logger_process.h"
#include "logger_fork_fd.h"
#include "logger_cleanup.h"
#include <errno.h>

static _Thread_local int entered;
static _Thread_local int prepared;

int logger_libc_busy(void)
{
	return entered;
}

static int begin(void)
{
	int error = logger_process_require();
	if (error)
		return -error;
	if (entered)
		return EDEADLK;
	struct logger_runtime *runtime = logger_runtime_current();
	error = atomic_load_explicit(&runtime->resource_error,
				     memory_order_acquire);
	if (!error)
		error = __cleanup_lockdep_mutex_lock(
			&runtime->format_mu, LOGGER_LOCK_RUNTIME_FORMAT);
	if (!error) {
		/* 等待锁期间另一线程可能封闭了进程域，取得锁后必须复核。 */
		error = atomic_load_explicit(&runtime->resource_error,
					     memory_order_acquire);
		if (error) {
			int released = __cleanup_lockdep_mutex_unlock(
				&runtime->format_mu,
				LOGGER_LOCK_RUNTIME_FORMAT);
			if (released)
				logger_fork_resource_poison(released);
		} else {
			entered = 1;
		}
	}
	return error;
}

static int end(int error)
{
	int saved = errno;
	int rc = __cleanup_lockdep_mutex_unlock(
		&logger_runtime_current()->format_mu,
		LOGGER_LOCK_RUNTIME_FORMAT);

	entered = 0;
	if (rc)
		logger_fork_resource_poison(rc);
	errno = saved;
	/* 主操作错误优先；若主操作成功，解锁失败也不能伪装成成功。 */
	return error ? error : rc;
}

static int finish_int(int result, int saved, int fallback)
{
	int error = result < 0 ? (errno ? errno : fallback) : 0;
	error = end(error);
	errno = error ? error : saved;
	return error ? -1 : result;
}

int logger_libc_fork_prepare(void)
{
	prepared = 0;
	int rc = begin();
	if (!rc)
		prepared = 1;
	return rc;
}

void logger_libc_fork_parent(void)
{
	if (prepared)
		(void)end(0);
	prepared = 0;
}

void logger_libc_fork_child(void)
{
	/* 锁在不继承映射中；只能清理当前线程标记，不能操作旧 mutex。 */
	prepared = 0;
	entered = 0;
}

int logger_libc_vsnprintf(char *out, size_t cap, const char *fmt, va_list args)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return -1;
	}
	/* GNU %m 使用进入调用时的 errno，不能为错误检测将其清零。 */
	errno = saved;
	int result = vsnprintf(out, cap, fmt, args);
	return finish_int(result, saved, EILSEQ);
}

int logger_libc_vfprintf(FILE *out, const char *fmt, va_list args)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return -1;
	}
	errno = saved;
	int result = vfprintf(out, fmt, args);
	return finish_int(result, saved, EIO);
}

struct tm *logger_libc_localtime_r(const time_t *time, struct tm *out)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return NULL;
	}
	errno = 0;
	struct tm *result = localtime_r(time, out);
	error = end(result ? 0 : (errno ? errno : EOVERFLOW));
	errno = error ? error : saved;
	return error ? NULL : result;
}

struct tm *logger_libc_gmtime_r(const time_t *time, struct tm *out)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return NULL;
	}
	errno = 0;
	struct tm *result = gmtime_r(time, out);
	error = end(result ? 0 : (errno ? errno : EOVERFLOW));
	errno = error ? error : saved;
	return error ? NULL : result;
}

size_t logger_libc_strftime(char *out, size_t cap, const char *fmt,
			    const struct tm *tm)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return 0;
	}
	errno = 0;
	size_t result = strftime(out, cap, fmt, tm);
	/* strftime 的空输出或容量不足也可以返回零而不设置 errno。 */
	error = end(result ? 0 : errno);
	errno = error ? error : saved;
	return error ? 0 : result;
}

int logger_libc_fflush(FILE *out)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return EOF;
	}
	errno = 0;
	int result = fflush(out);
	return finish_int(result, saved, EIO);
}

int logger_libc_fputc(int c, FILE *out)
{
	int saved = errno;
	int error = begin();
	if (error) {
		errno = error;
		return EOF;
	}
	errno = 0;
	int result = fputc(c, out);
	return finish_int(result, saved, EIO);
}

int logger_libc_snprintf(char *out, size_t cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc = logger_libc_vsnprintf(out, cap, fmt, args);
	va_end(args);
	return rc;
}

int logger_libc_fprintf(FILE *out, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc = logger_libc_vfprintf(out, fmt, args);
	va_end(args);
	return rc;
}
