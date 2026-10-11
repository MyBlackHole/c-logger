#include "console.h"
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

/* 每次操作只读取一个 config snapshot：并发 init 不会把新 verbosity 与旧 color 混用。
 * 独立 setter 只对自己负责的 bit 做 CAS。 */
#define COLOR_BITS 2u
#define COLOR_MASK 3u
static _Atomic unsigned g_config = (CONSOLE_NORMAL << COLOR_BITS) |
				   CONSOLE_COLOR_AUTO;
static _Atomic int g_console_lock_error;
static pthread_mutex_t g_console_mu = PTHREAD_MUTEX_INITIALIZER;

static int finish(logger_scope_t *scope, int rc)
{
	return logger_scope_end(scope, rc) < 0 ? -1 : 0;
}

void console_init(const console_config_t *c)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	int rc = 0;
	if (c) {
		_Static_assert(CONSOLE_CONFIG_SIZE == sizeof(console_config_t),
			       "Console v1 config layout changed");
		console_config_t config;
		uint32_t size, version;
		memcpy(&size, (const unsigned char *)c, sizeof(size));
		memcpy(&version, (const unsigned char *)c + sizeof(size),
		       sizeof(version));
		rc = logger_uapi_copy_struct(&config, sizeof(config), c, size,
					     version, CONSOLE_CONFIG_VERSION);
		if (!rc &&
		    ((unsigned)config.verbosity > CONSOLE_DEBUG ||
		     (unsigned)config.color > CONSOLE_COLOR_NEVER))
			rc = -EINVAL;
		if (!rc)
			atomic_store_explicit(&g_config,
					      ((unsigned)config.verbosity
					       << COLOR_BITS) |
						      (unsigned)config.color,
					      memory_order_relaxed);
	}
	(void)finish(&scope, rc);
}

static void set_config(unsigned value, int color)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	int rc = value > (unsigned)(color ? CONSOLE_COLOR_NEVER :
					    CONSOLE_DEBUG) ?
			 -EINVAL :
			 0;
	if (!rc) {
		unsigned old = atomic_load_explicit(&g_config,
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
	(void)finish(&scope, rc);
}

void console_set_verbosity(console_verbosity_t value)
{
	set_config((unsigned)value, 0);
}
void console_set_color(console_color_mode_t value)
{
	set_config((unsigned)value, 1);
}

static int tty(int fd)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return 0;
	int yes = isatty(fd);
	int rc = yes ? 0 : -(errno ? errno : ENOTTY);
	(void)finish(&scope, rc);
	return yes;
}
int console_stdout_is_tty(void)
{
	return tty(STDOUT_FILENO);
}
int console_stderr_is_tty(void)
{
	return tty(STDERR_FILENO);
}

static int color_enabled(unsigned config, FILE *out)
{
	unsigned color = config & COLOR_MASK;
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

/* scope 覆盖全部 libc 调用、本库 mutex 和 FILE 内部锁。
 * cancellation 发生时不会回滚已经完成或部分完成的输出；
 * 也不承诺与 host 外部 flockfile() 建立额外 lock order。 */
static int write_v(FILE *out, const char *label, const char *ansi,
		   unsigned config, const char *fmt, va_list ap)
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
	logger_sigpipe_guard_t sigpipe_guard;
	rc = logger_sigpipe_begin(&sigpipe_guard);
	int guarded = !rc;
	int saw_epipe = 0;
	if (guarded) {
		if (label) {
			int n = color_enabled(config, out) && ansi ?
					fprintf(out, "%s%s:\033[0m ", ansi, label) :
					fprintf(out, "%s: ", label);
			stdio_error(n < 0, &rc, &saw_epipe);
		}
		if (!rc)
			stdio_error(vfprintf(out, fmt, ap) < 0, &rc, &saw_epipe);
		if (!rc && label)
			stdio_error(fputc('\n', out) == EOF, &rc, &saw_epipe);
		stdio_error(fflush(out) != 0, &rc, &saw_epipe);
	}
	int unlock_rc = RELEASE_ERR(pthread_mutex_console,
				    &console_guard);
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
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	unsigned config = atomic_load_explicit(&g_config, memory_order_relaxed);
	va_list ap;
	va_start(ap, fmt);
	int rc = write_v(stdout, NULL, NULL, config, fmt, ap);
	va_end(ap);
	return finish(&scope, rc);
}

#define DIAGNOSTIC(function, minimum, label, color)                            \
	int function(const char *fmt, ...)                                     \
	{                                                                      \
		logger_scope_t scope;                                          \
		if (logger_scope_begin(&scope))                                \
			return -1;                                             \
		unsigned config =                                              \
			atomic_load_explicit(&g_config, memory_order_relaxed); \
		int rc = !fmt ? -EINVAL : 0;                                   \
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
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	unsigned config = atomic_load_explicit(&g_config, memory_order_relaxed);
	int rc = !fmt ? -EINVAL : 0;
	if (!rc && (config >> COLOR_BITS) >= CONSOLE_DEBUG) {
		const char *base = file ? strrchr(file, '/') : NULL;
		base = base ? base + 1 : (file ? file : "?");
		char message[3072], merged[4096];
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(message, sizeof(message), fmt, ap);
		va_end(ap);
		if (n < 0)
			rc = -EILSEQ;
		else if ((size_t)n >= sizeof(message))
			rc = -EOVERFLOW;
		else {
			n = snprintf(merged, sizeof(merged), "%s:%d %s() %s",
				     base, line, func ? func : "?", message);
			if (n < 0)
				rc = -EILSEQ;
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
			ACQUIRE(pthread_mutex_console_checked, console_guard)(&g_console_mu);
			rc = ACQUIRE_ERR(pthread_mutex_console_checked, &console_guard);
			if (!rc) {
				logger_sigpipe_guard_t sigpipe_guard;
				rc = logger_sigpipe_begin(&sigpipe_guard);
				int guarded = !rc, saw_epipe = 0;
				if (guarded) {
					stdio_error(fprintf(stderr, "DEBUG: %s\n", merged) < 0,
						    &rc, &saw_epipe);
					stdio_error(fflush(stderr) != 0, &rc,
						    &saw_epipe);
				}
				int unlock_rc = RELEASE_ERR(
					pthread_mutex_console,
					&console_guard);
				if (unlock_rc) {
					atomic_store_explicit(&g_console_lock_error,
							      -unlock_rc,
							      memory_order_release);
					if (!rc)
						rc = unlock_rc;
				}
				if (guarded)
					rc = logger_sigpipe_end(&sigpipe_guard, rc,
							 saw_epipe);
			}
		}
	}
	return finish(&scope, rc);
}
