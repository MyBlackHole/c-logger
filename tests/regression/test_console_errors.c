#define _GNU_SOURCE
#include "logger.h"
#include "console.h"
#include "logger_runtime.h"
#include "support.h"
#include <stdarg.h>

static FILE *report;
#undef CHECK
#define CHECK(expr)                                                           \
	do {                                                                  \
		if (!(expr)) {                                                \
			fprintf(report ? report : stderr,                     \
				"%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, \
				#expr, errno);                                \
			exit(1);                                              \
		}                                                             \
	} while (0)

/* 相同源码分别链接生产静态归档和白盒归档；不声称包装 DSO 内部调用。 */
static int armed;
static int fail_setup;
static int fail_restore;
static int fail_write;
static int fail_lock;
static int fail_unlock;
static int tty_answer = -1;
static int tty_error;
static int query_output;
static unsigned changes;

int __real_pthread_setcancelstate(int, int *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_isatty(int);
int __real_vfprintf(FILE *, const char *, va_list);

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (armed && state == PTHREAD_CANCEL_DISABLE && fail_setup) {
		fail_setup = 0;
		return EAGAIN;
	}
	int rc = __real_pthread_setcancelstate(state, old);
	if (!rc && armed && state == PTHREAD_CANCEL_ENABLE && fail_restore) {
		fail_restore = 0;
		return EIO;
	}
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (armed && mu == &logger_runtime_current()->console_mu && fail_lock) {
		fail_lock = 0;
		return EAGAIN;
	}
	return __real_pthread_mutex_lock(mu);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	int rc = __real_pthread_mutex_unlock(mu);
	if (!rc && armed && mu == &logger_runtime_current()->console_mu &&
	    fail_unlock) {
		fail_unlock = 0;
		return EPERM;
	}
	return rc;
}

int __wrap_isatty(int fd)
{
	if (armed && tty_answer >= 0) {
		errno = tty_error;
		return tty_answer;
	}
	return __real_isatty(fd);
}

int __wrap_vfprintf(FILE *stream, const char *format, va_list args)
{
	if (armed && fail_write) {
		fail_write = 0;
		errno = ENOSPC;
		return -1;
	}
	return __real_vfprintf(stream, format, args);
}

static int call(unsigned op)
{
	console_config_t cfg = CONSOLE_DEFAULT_CONFIG();
	cfg.color = CONSOLE_COLOR_NEVER;
	cfg.verbosity = CONSOLE_DEBUG;
	switch (op) {
	case 0:
		return console_init(&cfg);
	case 1:
		return console_set_color(CONSOLE_COLOR_NEVER);
	case 2:
		return console_set_verbosity(CONSOLE_DEBUG);
	case 3:
		return console_stdout_is_tty(&query_output);
	case 4:
		return console_stderr_is_tty(&query_output);
	case 5:
		return console_print("text\n");
	case 6:
		return console_info("text");
	case 7:
		return console_warn("text");
	case 8:
		return console_error("text");
	case 9:
		return console_verbose("text");
	case 10:
		return console_debug("text");
	case 11:
		return console_debug_source("source.c", 7, "f", "text");
	default:
		CHECK(0);
		return -EINVAL;
	}
}

static void check(int rc, int error)
{
	CHECK(rc == -error && errno == EDOM);
	++changes;
}

/* 直接读取已交给文件描述符的字节，检查器不能替 Console 刷新 FILE 缓冲。
 * 每次核对完整增量，缺失、重复和额外输出都会失败。
 */
static void expect_output(FILE *sink, off_t *offset, const char *expected)
{
	int saved = errno;
	char actual[128];
	size_t length = strlen(expected);
	CHECK(length < sizeof(actual));
	ssize_t count = pread(fileno(sink), actual, sizeof(actual), *offset);
	CHECK(count >= 0 && (size_t)count == length);
	CHECK(!memcmp(actual, expected, length));
	*offset += (off_t)length;
	errno = saved;
}

static void cancel_matrix(FILE *sink, int setup)
{
	static const char *const output[] = {
		"text\n",
		"INFO: text\n",
		"WARN: text\n",
		"ERROR: text\n",
		"INFO: text\n",
		"DEBUG: text\n",
		"DEBUG: source.c:7 f() text\n"
	};
	off_t offset = 0;

	for (unsigned i = 0; i < 12; ++i) {
		errno = EDOM;
		if (!setup && i < 3) {
			/* 三个配置入口均从不同旧值开始；先观察旧配置，避免
			 * 相同值重复提交掩盖恢复失败时未提交或回滚的缺陷。
			 */
			console_config_t before = CONSOLE_DEFAULT_CONFIG();
			before.verbosity = i == 1 ? CONSOLE_DEBUG : CONSOLE_QUIET;
			before.color = i == 2 ? CONSOLE_COLOR_NEVER :
					       CONSOLE_COLOR_ALWAYS;
			check(console_init(&before), 0);
			check(console_debug("committed"), 0);
			expect_output(sink, &offset,
				      i == 1 ? "\033[90mDEBUG:\033[0m committed\n" : "");
		}
		query_output = 7;
		fail_setup = setup;
		fail_restore = !setup;
		errno = EDOM;
		check(call(i), setup ? EAGAIN : EIO);
		CHECK(!fail_setup && !fail_restore);
		if (i == 3 || i == 4)
			CHECK(query_output == 7);
		/* 恢复失败已经写出的记录仍须恰好出现一次；设置取消失败
		 * 则不得输出。这里在后继调用之前检查，不能由重试补足。
		 */
		expect_output(sink, &offset, !setup && i >= 5 ? output[i - 5] : "");
		if (!setup && i < 3) {
			/* 成功重试前观察新配置；若提交被推迟到 finish 成功
			 * 之后，旧静默级别或旧颜色会使精确字节断言失败。
			 */
			check(console_debug("committed"), 0);
			expect_output(sink, &offset, "DEBUG: committed\n");
		}
		/* 同进程后继调用可用，不能留下重入或取消状态污染。 */
		check(call(i), 0);
		expect_output(sink, &offset, i >= 5 ? output[i - 5] : "");
	}
}

int main(int argc, char **argv)
{
	report = stderr;
	CHECK(argc == 2);
	CHECK(logger_process_init() == 0);
	console_config_t cfg = CONSOLE_DEFAULT_CONFIG();
	cfg.color = CONSOLE_COLOR_NEVER;
	cfg.verbosity = CONSOLE_DEBUG;
	CHECK(console_init(&cfg) == 0);
	FILE *sink = tmpfile();
	CHECK(sink);
	FILE *saved_out = stdout;
	FILE *saved_err = stderr;
	stdout = stderr = sink;
	armed = 1;
	tty_answer = 0;
	tty_error = ENOTTY;
	errno = EDOM;
	if (!strcmp(argv[1], "setup") || !strcmp(argv[1], "restore")) {
		cancel_matrix(sink, !strcmp(argv[1], "setup"));
	} else if (!strcmp(argv[1], "config-primary")) {
		fail_restore = 1;
		check(console_init(NULL), EINVAL);
		CHECK(!fail_restore);
	} else if (!strcmp(argv[1], "write-primary")) {
		fail_write = fail_restore = 1;
		check(console_print("no-output"), ENOSPC);
		CHECK(!fail_write && !fail_restore);
	} else if (!strcmp(argv[1], "tty-primary")) {
		tty_error = EBADF;
		fail_restore = 1;
		query_output = 7;
		check(console_stdout_is_tty(&query_output), EBADF);
		CHECK(query_output == 7 && !fail_restore);
	} else if (!strcmp(argv[1], "tty-terminal")) {
		tty_answer = 1;
		tty_error = EIO; /* 成功结果不读取未定义的残留错误。 */
		check(console_stdout_is_tty(&query_output), 0);
		CHECK(query_output == 1);
	} else if (!strcmp(argv[1], "tty-nonterminal")) {
		query_output = 7;
		check(console_stderr_is_tty(&query_output), 0);
		CHECK(query_output == 0);
	} else if (!strcmp(argv[1], "tty-missing-error")) {
		tty_error = 0;
		query_output = 7;
		check(console_stdout_is_tty(&query_output), EIO);
		CHECK(query_output == 7);
	} else if (!strcmp(argv[1], "lock")) {
		fail_lock = 1;
		check(console_print("no-output"), EAGAIN);
		CHECK(!fail_lock);
		check(console_print("after-failure"), 0);
	} else {
		CHECK(!strcmp(argv[1], "unlock"));
		fail_unlock = 1;
		check(console_print("written-before-error"), EPERM);
		CHECK(!fail_unlock);
		check(console_print("poisoned"), EPERM);
	}
	armed = 0;
	stdout = saved_out;
	stderr = saved_err;
	CHECK(fclose(sink) == 0);
	CHECK(changes > 0);
	printf("CONSOLE_ERROR_PASS=%s assertions=%u\n", argv[1], changes);
	return 0;
}
