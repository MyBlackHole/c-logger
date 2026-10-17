#define _GNU_SOURCE
#include <errno.h>
#include "default_fixture.h"
static logger_t *default_owner;
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "console.h"
#undef CONSOLE_DEBUG
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* 错误的子进程 API 调用必须在锁、once、分配或 I/O 前返回。
 * 这些包装函数仅链接到此可执行文件。_exit 代码 90..93
 * 用于区分被禁止操作与 errno 或返回值断言。
 */
/* 即使原始 SYS_fork 防御实验也保持探针标志为原子变量；
 * 该实验也有意绕过 Sanitizer 的 fork 拦截器。
 */
static _Atomic int strict_child;
static _Atomic int fail_registration, pause_registration, pause_reader,
	pause_console;
static _Atomic int entered, release_pause;
static _Atomic int release_console_at_prepare;
static pthread_t console_thread;
static _Atomic int console_completed, console_joined;
static _Thread_local int pause_console_flush_lock;
static _Atomic unsigned registrations;
static _Atomic unsigned foreign_registrations;
static _Atomic int omit_registration_observation;
static logger_t *instance;
static logger_config_t cfg;

int __real_pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int __wrap_pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void))
{
	/* 只观测本库的真实注册；宿主及检测器的处理函数不属于注入目标。
	 * 必须在故障和暂停开关之前分流，不能仅修正最终计数。
	 */
	if (c != logger_process_invalidate_child) {
		atomic_fetch_add(&foreign_registrations, 1);
		return __real_pthread_atfork(a, b, c);
	}
	if (strict_child)
		_exit(90);
	if (!atomic_load(&omit_registration_observation))
		atomic_fetch_add(&registrations, 1);
	if (atomic_load(&fail_registration))
		return ENOMEM;
	if (atomic_exchange(&pause_registration, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_pause);
	}
	return __real_pthread_atfork(a, b, c);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *p)
{
	if (atomic_load(&release_console_at_prepare) &&
	    p == &logger_runtime_current()->format_mu) {
		/* 新协议在 fork 前等待 libc 格式敏感区间结束；精确到达
		 * prepare 的锁入口后释放夹具，不再等 fork 返回才放行。
		 */
		atomic_store(&release_console_at_prepare, 0);
		atomic_store(&release_pause, 1);
	}
	if (strict_child)
		_exit(90);
	int rc = __real_pthread_mutex_lock(p);
	/* console_print 在正文格式化之后还会为 fflush 再次取锁。
	 * 只暂停这个最后区间，避免 prepare 持锁 join 时等待再次取锁。
	 */
	if (!rc && pause_console_flush_lock &&
	    p == &logger_runtime_current()->format_mu) {
		pause_console_flush_lock = 0;
		atomic_store(&entered, 1);
		wait_flag(&release_pause);
	}
	return rc;
}
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *p)
{
	if (strict_child)
		_exit(90);
	return __real_pthread_rwlock_rdlock(p);
}
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *p)
{
	if (strict_child)
		_exit(90);
	return __real_pthread_rwlock_wrlock(p);
}
int __real_pthread_once(pthread_once_t *, void (*)(void));
int __wrap_pthread_once(pthread_once_t *p, void (*f)(void))
{
	if (strict_child)
		_exit(90);
	return __real_pthread_once(p, f);
}
void *__real_logger_runtime_calloc(size_t, size_t);
void *__wrap_logger_runtime_calloc(size_t n, size_t z)
{
	if (strict_child)
		_exit(91);
	return __real_logger_runtime_calloc(n, z);
}
int __real_logger_runtime_free(void *);
int __wrap_logger_runtime_free(void *p)
{
	if (strict_child)
		_exit(91);
	return __real_logger_runtime_free(p);
}
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			  void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
			  void *(*f)(void *), void *p)
{
	if (strict_child)
		_exit(91);
	return __real_pthread_create(t, a, f, p);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
	if (strict_child)
		_exit(92);
	return __real_write(fd, p, n);
}
int __real_close(int);
int __wrap_close(int fd)
{
	if (strict_child)
		_exit(92);
	return __real_close(fd);
}
int __real_vsnprintf(char *, size_t, const char *, va_list);
int __wrap_vsnprintf(char *p, size_t n, const char *fmt, va_list a)
{
	if (strict_child)
		_exit(93);
	return __real_vsnprintf(p, n, fmt, a);
}
int __real_vfprintf(FILE *, const char *, va_list);
int __wrap_vfprintf(FILE *f, const char *fmt, va_list a)
{
	if (strict_child)
		_exit(93);
	int result = __real_vfprintf(f, fmt, a);
	if (atomic_exchange(&pause_console, 0))
		pause_console_flush_lock = 1;
	return result;
}
void __real_logger_vlog_internal(logger_t *, logger_level_t, const char *,
				 const char *, int, const char *, const char *,
				 va_list);
void __wrap_logger_vlog_internal(logger_t *l, logger_level_t v, const char *m,
				 const char *f, int line, const char *fn,
				 const char *fmt, va_list a)
{
	if (strict_child)
		_exit(93);
	if (atomic_exchange(&pause_reader, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_pause);
	}
	__real_logger_vlog_internal(l, v, m, f, line, fn, fmt, a);
}

/* 子进程断言中不使用 CHECK、stdio 或 malloc。 */
#define CHILD_POSIX(expr, code)                      \
	do {                                         \
		errno = 0;                           \
		if ((expr) != -1 || errno != ECHILD) \
			_exit(code);                 \
	} while (0)
#define CHILD_VOID(expr, code)       \
	do {                         \
		errno = 0;           \
		expr;                \
		if (errno != ECHILD) \
			_exit(code); \
	} while (0)
/* 新状态接口的入口拒绝不得污染调用方 errno。 */
#define CHILD_STATUS(call, code)                        \
	do {                                            \
		errno = EDOM;                           \
		if ((call) != -ECHILD || errno != EDOM) \
			_exit(code);                    \
	} while (0)

static void child_guard_matrix(void)
{
	strict_child = 1;
	(void)alarm(5);
	if (logger_default_set(NULL, NULL) != -ECHILD)
		_exit(10);
	errno = 0;
	if (logger_create(&cfg, &(logger_t *){ NULL }) != -ECHILD)
		_exit(11);
	CHILD_VOID(LOG_INFO("INHERITED_FORBIDDEN"), 14);
	CHILD_STATUS(logger_default_flush(), 15);
	CHILD_STATUS(logger_default_reopen(), 16);
	CHILD_VOID(logger_set_level(LOGGER_DEBUG), 17);
	if (logger_default_set(NULL, NULL) != -ECHILD)
		_exit(18);
	logger_metrics_t m = { .enqueued = 777 };
	logger_io_metrics_t io = { .emitted_records = 777 };
	CHILD_VOID(logger_get_global_metrics(&m), 19);
	CHILD_VOID(logger_get_global_io_metrics(&io), 20);
	if (m.enqueued || io.emitted_records)
		_exit(21);
	errno = 0;
	if (logger_global_dropped() != 0 || errno != ECHILD)
		_exit(22);

	/* 仅检查来源的情形中，此毒化指针检查在任何解引用前拒绝。
	 * 显式实例情形使用真实继承的已分配实例。
	 */
	logger_t *l = instance ? instance : (logger_t *)(uintptr_t)1;
	CHILD_VOID(LOGGER_INFO(l, "fork", "INHERITED_FORBIDDEN"), 23);
	CHILD_VOID(logger_log_source(l, LOGGER_INFO, (logger_source_t){ 0 },
				     "forbidden"),
		   24);
	if (logger_log_sync_status(l, LOGGER_INFO, "fork", NULL, 0, NULL,
				   "forbidden") != -ECHILD)
		_exit(25);
	CHILD_STATUS(logger_flush(l), 26);
	errno = EDOM;
	(void)logger_flush(l);
	if (errno != EDOM)
		_exit(27);
	CHILD_STATUS(logger_reopen(l), 28);
	CHILD_VOID(logger_set_instance_level(l, LOGGER_ERROR), 31);
	CHILD_VOID(logger_get_metrics(l, &m), 32);
	CHILD_VOID(logger_get_io_metrics(l, &io), 33);
	if (m.enqueued || io.emitted_records)
		_exit(34);
	errno = 0;
	if (logger_get_state(l) != LOGGER_STATE_STOPPED || errno != ECHILD)
		_exit(35);
	errno = 0;
	if (logger_dropped(l) != 0 || errno != ECHILD)
		_exit(36);
	logger_t *old = l;
	errno = EDOM;
	if (logger_destroy(&l) != -ECHILD || l != old || errno != EDOM)
		_exit(37);
	CHILD_VOID(
		logger_context_set(&(logger_context_t){ .request_id = "new" }),
		38);
	CHILD_VOID(logger_context_clear(), 39);
	errno = 0;
	logger_context_t ctx = logger_context_get();
	if (ctx.request_id || ctx.session_id || ctx.trace_id || errno != ECHILD)
		_exit(40);

	console_config_t console_cfg = CONSOLE_DEFAULT_CONFIG();
	CHILD_STATUS(console_init(&console_cfg), 41);
	CHILD_STATUS(console_set_verbosity(CONSOLE_QUIET), 42);
	CHILD_STATUS(console_set_color(CONSOLE_COLOR_NEVER), 43);
	CHILD_STATUS(console_print("INHERITED_FORBIDDEN"), 44);
	CHILD_STATUS(console_info("forbidden"), 45);
	CHILD_STATUS(console_warn("forbidden"), 46);
	CHILD_STATUS(console_error("forbidden"), 47);
	CHILD_STATUS(console_verbose("forbidden"), 48);
	CHILD_STATUS(console_debug("forbidden"), 49);
	CHILD_STATUS(console_debug_source(__FILE__, __LINE__, __func__,
					  "forbidden"),
		     50);
	int tty = 9;
	CHILD_STATUS(console_stdout_is_tty(&tty), 51);
	CHILD_STATUS(console_stderr_is_tty(&tty), 52);
	if (tty != 9)
		_exit(53);

	if (logger_default_set(NULL, NULL) != -ECHILD)
		_exit(66);
	_exit(0);
}

static void wait_child(pid_t p)
{
	int status;
	while (waitpid(p, &status, 0) < 0)
		CHECK(errno == EINTR);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		fprintf(stderr, "child status=%d (exit=%d)\n", status,
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void early_child(void)
{
	int previous = strict_child;
	strict_child = 1;
	/* 在库处理函数之前注册：必须由不可变 PID 所有权，
	 * 而非处理函数顺序，阻止默认绑定接触继承的运行域。
	 */
	if (logger_default_set(NULL, NULL) != -ECHILD)
		_exit(70);
	/* 库后续 child handler 允许 close 已登记引用；拒绝的 API 本身仍禁止 I/O。 */
	strict_child = previous;
}
static void *global_writer(void *p)
{
	(void)p;
	LOG_INFO("PARENT_READER");
	return NULL;
}
static void *console_writer(void *p)
{
	(void)p;
	CHECK(console_print("parent console\n") == 0);
	CHECK(!pause_console_flush_lock);
	atomic_store(&console_completed, 1);
	return NULL;
}
/* 先于 Logger 注册，因 prepare 逆序执行而后于 Logger prepare 运行。
 * 保留进入 prepare 时 Console 正在格式化的真实交错；只有 Logger
 * 完成格式区间协调后才回收夹具线程。不能让已退出但未 join 的线程
 * 记录进入子进程，也不能通过保持线程存活触发 TSan 的忽略模式。
 */
static void console_join_prepare(void)
{
	CHECK(atomic_load(&entered));
	CHECK(!atomic_load(&release_console_at_prepare));
	CHECK(!pthread_join(console_thread, NULL));
	CHECK(atomic_load(&console_completed));
	atomic_store(&console_joined, 1);
}

static void *initializer(void *p)
{
	(void)p;
	CHECK(logger_process_init() == 0);
	CHECK(fixture_default_create(&cfg, &default_owner) == 0);
	return NULL;
}
static void *lock_holder(void *p)
{
	pthread_mutex_t *mu = p;
	CHECK(!pthread_mutex_lock(mu));
	atomic_store(&entered, 1);
	wait_flag(&release_pause);
	CHECK(!pthread_mutex_unlock(mu));
	return NULL;
}

static void registration_failure(const char *mode)
{
	atomic_store(&fail_registration, 1);
	CHECK(logger_process_init() == -ENOMEM);
	errno = 0;
	if (!strcmp(mode, "register-global"))
		CHECK(logger_default_set(NULL, NULL) == -ENOMEM);
	else if (!strcmp(mode, "register-explicit"))
		CHECK(logger_create(&cfg, &(logger_t *){ NULL }) == -ENOMEM);
	else if (!strcmp(mode, "register-console"))
		CHECK(console_print("forbidden") == -ENOMEM && errno == 0);
	else if (!strcmp(mode, "register-context")) {
		logger_context_set(&(logger_context_t){ 0 });
		CHECK(errno == ENOMEM);
	} else
		CHECK(0);
	CHECK(access("out.log", F_OK) != 0);
	/* 注册失败不会自动清除，不允许静默地无保护重试。 */
	atomic_store(&fail_registration, 0);
	CHECK(logger_create(&cfg, &(logger_t *){ NULL }) == -ENOMEM);
	CHECK(logger_default_set(NULL, NULL) == -ENOMEM);
	CHECK(atomic_load(&registrations) == 1);
}

/* 与 Logger 的回调身份明确不同，不能被故障/暂停开关截获。 */
static void foreign_child(void)
{
}

static void check_foreign_registration(void)
{
	unsigned before = atomic_load(&foreign_registrations);

	CHECK(atomic_load(&registrations) == 0);
	atomic_store(&fail_registration, 1);
	atomic_store(&pause_registration, 1);
	CHECK(pthread_atfork(NULL, NULL, foreign_child) == 0);
	CHECK(atomic_load(&foreign_registrations) == before + 1);
	CHECK(atomic_load(&registrations) == 0);
	CHECK(atomic_load(&fail_registration) == 1);
	CHECK(atomic_load(&pause_registration) == 1);
	CHECK(atomic_load(&entered) == 0);
	atomic_store(&fail_registration, 0);
	atomic_store(&pause_registration, 0);
	printf("ATFORK_FOREIGN_BEFORE_MAIN=%u\n", before);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	check_foreign_registration();
	/* 仅供探针自检的负向对照，不进入生产库或修改正常断言。 */
	if (getenv("LOGGER_TEST_OMIT_ATFORK_OBSERVATION"))
		atomic_store(&omit_registration_observation, 1);
	const char *mode = argv[1];
	const char *const modes[] = {
		"global",	    "explicit",	       "console-only",
		"context-only",	    "emit-held",       "progress-held",
		"console-held",	    "reader-held",     "registration-window",
		"earlier-handler",  "after-shutdown",  "bypass-handler",
		"prefork",	    "register-global", "register-explicit",
		"register-console", "register-context"
	};
	int known = 0;
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
		known |= !strcmp(mode, modes[i]);
	CHECK(known);
	cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.queue_capacity = 128;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	unlink("out.log");
	if (!strncmp(mode, "register-", 9)) {
		registration_failure(mode);
		puts("registration failure rejected before resources");
		return 0;
	}
	if (!strcmp(mode, "prefork")) {
		/* 单线程，且 fork 前没有使用库运行时。 */
		pid_t p = fork();
		CHECK(p >= 0);
		if (!p) {
			if (logger_process_init())
				_exit(9);
			logger_t *l = NULL;
			(void)logger_create(&cfg, &l);
			if (!l)
				_exit(10);
			LOGGER_INFO(l, "child", "PREFORK_CHILD");
			if (logger_flush(l))
				_exit(11);
			logger_destroy(&l);
			_exit(0);
		}
		wait_child(p);
		CHECK(file_contains("out.log", "PREFORK_CHILD"));
		puts("single-threaded fork before library use passed");
		return 0;
	}
	if (!strcmp(mode, "earlier-handler"))
		CHECK(!__real_pthread_atfork(NULL, NULL, early_child));
	/* Console 的宿主 prepare 必须先注册，保留原锁协调交错。 */
	if (!strcmp(mode, "console-held"))
		CHECK(!__real_pthread_atfork(console_join_prepare, NULL, NULL));
	if (strcmp(mode, "registration-window"))
		CHECK(logger_process_init() == 0);
	pthread_t thread;
	int has_thread = 0;
	if (!strcmp(mode, "registration-window")) {
		atomic_store(&pause_registration, 1);
		CHECK(!pthread_create(&thread, NULL, initializer, NULL));
		has_thread = 1;
		wait_flag(&entered);
	} else if (!strcmp(mode, "console-only") ||
		   !strcmp(mode, "console-held")) {
		console_config_t c = CONSOLE_DEFAULT_CONFIG();
		console_init(&c);
		if (!strcmp(mode, "console-held")) {
			atomic_store(&pause_console, 1);
			CHECK(!pthread_create(&console_thread, NULL,
					      console_writer, NULL));
			wait_flag(&entered);
		}
	} else if (!strcmp(mode, "context-only")) {
		logger_context_set(&(logger_context_t){
			.request_id = "inherited-secret-id" });
	} else if (!strcmp(mode, "explicit") || !strcmp(mode, "emit-held") ||
		   !strcmp(mode, "progress-held")) {
		CHECK(logger_create(&cfg, &instance) == 0);
		CHECK(instance != NULL);
		if (strcmp(mode, "explicit")) {
			pthread_mutex_t *mu = !strcmp(mode, "emit-held") ?
						      &instance->emit_mu :
						      &instance->progress_mu;
			CHECK(!pthread_create(&thread, NULL, lock_holder, mu));
			has_thread = 1;
			wait_flag(&entered);
		}
	} else {
		CHECK(!fixture_default_create(&cfg, &default_owner));
		if (!strcmp(mode, "reader-held")) {
			atomic_store(&pause_reader, 1);
			CHECK(!pthread_create(&thread, NULL, global_writer,
					      NULL));
			has_thread = 1;
			wait_flag(&entered);
		} else if (!strcmp(mode, "after-shutdown"))
			fixture_default_destroy(&default_owner);
	}
	if (!strcmp(mode, "console-held"))
		atomic_store(&release_console_at_prepare, 1);
	if (getenv("LOGGER_TEST_DUPLICATE_ATFORK")) {
		/* 真正再次调用注册入口，不是直接把计数器加一。 */
		void (*child)(void) = logger_process_invalidate_child;
		CHECK(pthread_atfork(NULL, NULL, child) == 0);
	}
	pid_t p;
	if (!strcmp(mode, "bypass-handler")) {
#ifdef SYS_fork
		p = (pid_t)syscall(
			SYS_fork); /* 仅测试 PID 防御，不是受支持 API */
#else
		return 77;
#endif
	} else
		p = fork();
	CHECK(p >= 0);
	if (!p)
		child_guard_matrix();
	atomic_store(&release_pause, 1);
	if (!strcmp(mode, "console-held"))
		CHECK(atomic_load(&console_joined));
	if (has_thread)
		CHECK(!pthread_join(thread, NULL));
	wait_child(p);
	if (instance) {
		LOGGER_INFO(instance, "parent", "PARENT_CONTINUES");
		CHECK(!logger_flush(instance));
		logger_destroy(&instance);
	}
	fixture_default_destroy(&default_owner);
	fprintf(stderr, "ATFORK_LOGGER_REGISTRATIONS=%u\n",
		atomic_load(&registrations));
	CHECK(atomic_load(&registrations) == 1);
	puts("fork misuse rejected before runtime access; parent unaffected");
	return 0;
}
