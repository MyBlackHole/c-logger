#include "logger_libc.h"
#include "console.h"
#include "logger_runtime.h"
#include "logger_scope.h"
#include "logger_cleanup.h"
#include "logger_uapi.h"
#include "logger_sigpipe.h"
#undef CONSOLE_DEBUG
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 每次操作只读取一个配置快照：并发初始化不会把新 verbosity 与旧 color 混用。
 * 独立设置函数只对自己负责的位做 CAS。
 */
#define COLOR_BITS 2u
#define COLOR_MASK 3u
#define g_config (logger_runtime_current()->console_config)
#define g_console_lock_error (logger_runtime_current()->console_lock_error)
#define g_console_mu (logger_runtime_current()->console_mu)

/* 公开接口统一返回负错误码；作用域仍负责准入、取消和主错误优先。 */
static int begin(struct logger_scope *scope)
{
	int saved = errno;
	int rc = logger_scope_begin(scope);

	errno = saved;
	return rc;
}

static int finish(struct logger_scope *scope, int rc)
{
	int saved = scope->saved_errno;

	rc = logger_scope_end(scope, rc);
	errno = saved;
	return rc;
}

int console_init(const console_config_t *c)
{
	struct logger_scope scope;
	int rc = begin(&scope);
	if (rc)
		return rc;
	rc = c ? 0 : -EINVAL;
	if (!rc) {
		_Static_assert(CONSOLE_CONFIG_SIZE == sizeof(console_config_t),
			       "Console config layout changed");
		console_config_t config;
		uint32_t size;
		uint32_t version;
		memcpy(&size, (const unsigned char *)c, sizeof(size));
		memcpy(&version, (const unsigned char *)c + sizeof(size),
		       sizeof(version));
		rc = logger_uapi_copy_struct(&config, sizeof(config), c, size,
					     version, CONSOLE_CONFIG_VERSION);
		if (!rc && ((unsigned int)config.verbosity > CONSOLE_DEBUG ||
			    (unsigned int)config.color > CONSOLE_COLOR_NEVER))
			rc = -EINVAL;
		if (!rc)
			atomic_store_explicit(
				&g_config,
				((unsigned int)config.verbosity << COLOR_BITS) |
					(unsigned int)config.color,
				memory_order_relaxed);
	}
	return finish(&scope, rc);
}

static int set_config(unsigned int value, int color)
{
	struct logger_scope scope;
	int rc = begin(&scope);
	if (rc)
		return rc;
	rc = value > (unsigned int)(color ? CONSOLE_COLOR_NEVER :
					    CONSOLE_DEBUG) ?
		     -EINVAL :
		     0;
	if (!rc) {
		unsigned int old = atomic_load_explicit(&g_config,
							memory_order_relaxed),
			     next;
		do {
			next = color ? (old & ~COLOR_MASK) | value :
				       (old & COLOR_MASK) |
					       (value << COLOR_BITS);
		} while (!atomic_compare_exchange_weak_explicit(
			&g_config, &old, next, memory_order_relaxed,
			memory_order_relaxed));
	}
	return finish(&scope, rc);
}

int console_set_verbosity(console_verbosity_t value)
{
	return set_config((unsigned int)value, 0);
}
int console_set_color(console_color_mode_t value)
{
	return set_config((unsigned int)value, 1);
}

static int tty(int fd, int *out)
{
	struct logger_scope scope;
	int rc = begin(&scope);
	if (rc)
		return rc;
	if (!out)
		return finish(&scope, -EINVAL);

	/*
	 * 只有 ENOTTY 表示成功取得“非终端”答案，EBADF 等仍是查询错误。
	 * 先清 errno，避免异常实现未设错误时把调用方旧 errno 当作本次错误。
	 */
	errno = 0;
	int yes = isatty(fd);
	int error = errno;
	rc = yes || error == ENOTTY ? 0 : -(error ? error : EIO);
	rc = finish(&scope, rc);
	/* 取消策略恢复也可能失败；所有收尾成功后才发布输出，失败不改旧值。 */
	if (!rc)
		*out = !!yes;
	return rc;
}
int console_stdout_is_tty(int *out)
{
	return tty(STDOUT_FILENO, out);
}
int console_stderr_is_tty(int *out)
{
	return tty(STDERR_FILENO, out);
}

static int color_enabled(unsigned int config, FILE *out)
{
	unsigned int color = config & COLOR_MASK;
	if (color != CONSOLE_COLOR_AUTO)
		return color == CONSOLE_COLOR_ALWAYS;
	int saved = errno;
	int enabled = isatty(fileno(out));
	errno = saved; /* TTY 探测不能改变用户 printf %m 依赖的 errno。 */
	return enabled;
}

static void stdio_error(int failed, int *first_error, int *saw_epipe)
{
	if (!failed)
		return;
	int error = errno ? errno : EIO;
	if (error == EPIPE)
		*saw_epipe = 1;
	if (!*first_error)
		*first_error = -error;
}

/*
 * 作用域覆盖全部 libc 调用、本库互斥锁和 FILE 内部锁。
 * 发生取消时不会回滚已经完成或部分完成的输出；
 * 也不承诺与宿主外部 flockfile() 建立额外锁顺序。
 */
static int write_v(FILE *out, const char *label, const char *ansi,
		   unsigned int config, const char *fmt, va_list ap)
{
	if (!fmt)
		return -EINVAL;
	int lock_error = atomic_load_explicit(&g_console_lock_error,
					      memory_order_acquire);
	if (lock_error)
		return -lock_error;
	ACQUIRE(pthread_mutex_console_checked, console_guard)(&g_console_mu);
	int rc = ACQUIRE_ERR(pthread_mutex_console_checked, &console_guard);
	if (rc)
		return rc;
	struct logger_sigpipe_guard sigpipe_guard;
	rc = logger_sigpipe_begin(&sigpipe_guard);
	int guarded = !rc;
	int saw_epipe = 0;
	if (guarded) {
		if (label) {
			int n = color_enabled(config, out) && ansi ?
					logger_libc_fprintf(out,
							    "%s%s:\033[0m ",
							    ansi, label) :
					logger_libc_fprintf(out, "%s: ", label);
			stdio_error(n < 0, &rc, &saw_epipe);
		}
		if (!rc)
			stdio_error(logger_libc_vfprintf(out, fmt, ap) < 0, &rc,
				    &saw_epipe);
		if (!rc && label)
			stdio_error(logger_libc_fputc('\n', out) == EOF, &rc,
				    &saw_epipe);
		stdio_error(logger_libc_fflush(out) != 0, &rc, &saw_epipe);
	}
	int unlock_rc = RELEASE_ERR(pthread_mutex_console, &console_guard);
	if (unlock_rc) {
		atomic_store_explicit(&g_console_lock_error, -unlock_rc,
				      memory_order_release);
		if (!rc)
			rc = unlock_rc;
	}
	return guarded ? logger_sigpipe_end(&sigpipe_guard, rc, saw_epipe) : rc;
}

int console_print(const char *fmt, ...)
{
	struct logger_scope scope;
	int rc = begin(&scope);
	if (rc)
		return rc;
	unsigned int config =
		atomic_load_explicit(&g_config, memory_order_relaxed);
	va_list ap;
	va_start(ap, fmt);
	rc = write_v(stdout, NULL, NULL, config, fmt, ap);
	va_end(ap);
	return finish(&scope, rc);
}

#define DIAGNOSTIC(function, minimum, label, color)                            \
	int function(const char *fmt, ...)                                     \
	{                                                                      \
		struct logger_scope scope;                                     \
		int rc = begin(&scope);                                        \
		if (rc)                                                        \
			return rc;                                             \
		unsigned int config =                                          \
			atomic_load_explicit(&g_config, memory_order_relaxed); \
		rc = !fmt ? -EINVAL : 0;                                       \
		if (!rc && (int)(config >> COLOR_BITS) >= (int)(minimum)) {    \
			va_list ap;                                            \
			va_start(ap, fmt);                                     \
			rc = write_v(stderr, label, color, config, fmt, ap);   \
			va_end(ap);                                            \
		}                                                              \
		return finish(&scope, rc);                                     \
	}
DIAGNOSTIC(console_info, CONSOLE_NORMAL, "INFO", "\033[36m")
DIAGNOSTIC(console_warn, CONSOLE_QUIET, "WARN", "\033[33m")
DIAGNOSTIC(console_error, CONSOLE_QUIET, "ERROR", "\033[31m")
DIAGNOSTIC(console_verbose, CONSOLE_VERBOSE, "INFO", "\033[36m")
DIAGNOSTIC(console_debug, CONSOLE_DEBUG, "DEBUG", "\033[90m")
#undef DIAGNOSTIC

int console_debug_source(const char *file, int line, const char *func,
			 const char *fmt, ...)
{
	struct logger_scope scope;
	int rc = begin(&scope);
	if (rc)
		return rc;
	unsigned int config =
		atomic_load_explicit(&g_config, memory_order_relaxed);
	rc = !fmt ? -EINVAL : 0;
	if (!rc && (config >> COLOR_BITS) >= CONSOLE_DEBUG) {
		const char *base = file ? strrchr(file, '/') : NULL;
		base = base ? base + 1 : (file ? file : "?");
		char message[3072];
		char merged[4096];
		va_list ap;
		va_start(ap, fmt);
		int n = logger_libc_vsnprintf(message, sizeof(message), fmt,
					      ap);
		va_end(ap);
		if (n < 0)
			rc = -(errno ? errno : EILSEQ);
		else if ((size_t)n >= sizeof(message))
			rc = -EOVERFLOW;
		else {
			n = logger_libc_snprintf(merged, sizeof(merged),
						 "%s:%d %s() %s", base, line,
						 func ? func : "?", message);
			if (n < 0)
				rc = -(errno ? errno : EILSEQ);
			else if ((size_t)n >= sizeof(merged))
				rc = -EOVERFLOW;
		}
		if (!rc) {
			int lock_error = atomic_load_explicit(
				&g_console_lock_error, memory_order_acquire);
			if (lock_error)
				rc = -lock_error;
		}
		if (!rc) {
			ACQUIRE(pthread_mutex_console_checked, console_guard)
			(&g_console_mu);
			rc = ACQUIRE_ERR(pthread_mutex_console_checked,
					 &console_guard);
			if (!rc) {
				struct logger_sigpipe_guard sigpipe_guard;
				rc = logger_sigpipe_begin(&sigpipe_guard);
				int guarded = !rc;
				int saw_epipe = 0;
				if (guarded) {
					stdio_error(logger_libc_fprintf(
							    stderr,
							    "DEBUG: %s\n",
							    merged) < 0,
						    &rc, &saw_epipe);
					stdio_error(
						logger_libc_fflush(stderr) != 0,
						&rc, &saw_epipe);
				}
				int unlock_rc = RELEASE_ERR(
					pthread_mutex_console, &console_guard);
				if (unlock_rc) {
					atomic_store_explicit(
						&g_console_lock_error,
						-unlock_rc,
						memory_order_release);
					if (!rc)
						rc = unlock_rc;
				}
				if (guarded)
					rc = logger_sigpipe_end(&sigpipe_guard,
								rc, saw_epipe);
			}
		}
	}
	return finish(&scope, rc);
}
