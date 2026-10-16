#include "logger_libc.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "logger_cleanup.h"
#include "logger_lockdep.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* 只有生命周期控制操作获取 g_control_mu。
 * 普通调用通过 g_lifetime_lock 的读侧生命周期保护保证对象存活。
 * 这个读写锁保护不是引用计数：读者只在持有读锁时借用 g_logger。
 * 锁顺序固定为控制锁 -> 生命周期锁 -> 实例锁。
 * 持有生命周期读锁时禁止反向获取控制锁。
 */
#define g_control_mu (logger_runtime_current()->global_control_mu)
#define g_lifetime_lock (logger_runtime_current()->global_lifetime_lock)
#define g_logger (logger_runtime_current()->global_logger)
#define g_retired (logger_runtime_current()->global_retired)

enum global_phase {
	G_IDLE, G_STARTING, G_RUNNING, G_STOPPING, G_STOPPED, G_BROKEN
};
#define PHASE_BITS 3u
#define PHASE_MASK UINT64_C(7)
/* generation 与 phase 放在同一个原子票据中。
 * 若分开读取，读者可能把某一代 RUNNING 与另一代 generation 错配。
 * 每次真实初始化尝试都会消费一个 generation，即使候选对象构造失败。
 *
 * CAS 失败时需要 acquire 的生命周期转换，成功时也保留 acquire。
 * acq_rel/acquire 保留 release 发布，并明确满足本项目采用的 C11 约定，
 * 不依赖 release/acquire 在历史“失败序不得更强”措辞下的歧义。
 * 票据只控制准入，对象存活仍由 g_lifetime_lock 的生命周期锁保证。
 */
#define g_ticket (logger_runtime_current()->global_ticket)
/* 锁释放失败或拆除不完整会使全局生命周期失效。 */
#define g_lock_error (logger_runtime_current()->global_lock_error)
static _Thread_local int g_in_global;

typedef struct {
	int old_cancel;
	int saved_errno;
	int control_locked;
	int lifetime_locked;
	int proof_error; /* 当前线程可能仍持有未经验证的锁 */
} global_scope_t;

static enum global_phase phase_of(uint64_t ticket)
{
	return (enum global_phase)(ticket & PHASE_MASK);
}

static uint64_t generation_of(uint64_t ticket)
{
	return ticket >> PHASE_BITS;
}

static uint64_t with_phase(uint64_t ticket, enum global_phase phase)
{
	return (ticket & ~PHASE_MASK) | (uint64_t)phase;
}

static int global_lock_error(void)
{
	return atomic_load_explicit(&g_lock_error, memory_order_acquire);
}

/* 先发布不会自动清除的错误状态，再发布损坏阶段。
 * 若另一个控制方已在等待该锁，CAS 会保留更新的代次。
 */
static void global_lock_poison(int error)
{
	if (!error)
		return;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
		&g_lock_error, &expected, error,
		memory_order_release, memory_order_relaxed);
	uint64_t ticket = atomic_load_explicit(&g_ticket, memory_order_acquire);
	while (phase_of(ticket) != G_BROKEN &&
	       !atomic_compare_exchange_weak_explicit(
		       &g_ticket, &ticket, with_phase(ticket, G_BROKEN),
		       memory_order_acq_rel, memory_order_acquire)) {
	}
}

/* 初始化回滚绝不能撤销另一线程的证明失败。 */
static void rollback_ticket(uint64_t ticket, enum global_phase phase)
{
	(void)atomic_compare_exchange_strong_explicit(
		&g_ticket, &ticket, with_phase(ticket, phase),
		memory_order_release, memory_order_relaxed);
}

/* 调用时持有全局控制所有权，并持有未发布的候选对象。
 * 同一回执也涵盖伴随成功 free 的收尾错误；
 * 只有实际未完成释放时才保留此所有者。
 */
static int retire_candidate(logger_t *candidate)
{
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_CONTROL,
				   &g_control_mu);
	g_retired = candidate;
	logger_destroy_receipt_reset();
	int rc = logger_destroy(&candidate);
	if (logger_destroy_receipt_read())
		g_retired = NULL;
	else
		global_lock_poison(rc < 0 ? -rc : EUCLEAN);
	return rc;
}

/* 取消恢复生效前，必须先释放全部资源和生命周期保护。
 * 应用的取消清理处理函数随后仍可能调用 Logger。
 * 这里是取消延后，不是回滚，也不是 I/O 超时期限。
 * 显式实例 API 还会建立自己的内层作用域，并继承禁用状态。
 */
static int finish(global_scope_t *scope, int rc)
{
	int error;
	if (scope->proof_error) {
		/* 手动解锁已经失败。保留外层锁并继续禁用取消；
		 * 重复解锁不能证明所有权。
		 */
		errno = scope->proof_error;
		return -1;
	}
	if (scope->lifetime_locked) {
		error = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
		if (error) {
			global_lock_poison(error);
			/* 生命周期保护可能仍被持有。不要解锁控制方，
			 * 不要清除 TLS 重入标记，也不要恢复取消。
			 */
			errno = error;
			return -1;
		}
	}
	if (scope->control_locked) {
		error = __cleanup_lockdep_mutex_unlock(
			&g_control_mu, LOGGER_LOCK_GLOBAL_CONTROL);
		if (error) {
			global_lock_poison(error);
			errno = error;
			return -1;
		}
	}
	error = rc < 0 ? -rc : scope->saved_errno;
	g_in_global = 0;
	errno = error;
	(void)pthread_setcancelstate(scope->old_cancel, NULL);
	errno = error;
	return rc < 0 ? -1 : 0;
}

static int begin(global_scope_t *scope)
{
	/* 接触继承来的同步对象或 once 状态前，先检查进程标识。 */
	if (logger_process_is_child()) {
		errno = ECHILD;
		return -1;
	}
	/* 拒绝同线程直接重入，例如 printf 或符号插入回调再次调用 Logger。
	 * 这并不意味着信号处理函数或跨线程回调环是安全的，
	 * 也不允许 pthread_exit/longjmp 穿过 Logger 调用。
	 */
	if (g_in_global || logger_scope_busy()) {
		errno = EDEADLK;
		return -1;
	}
	int rc = logger_process_require();
	if (rc) {
		errno = -rc;
		return -1;
	}
	*scope = (global_scope_t){ .saved_errno = errno };
	rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &scope->old_cancel);
	if (rc) {
		errno = rc;
		return -1;
	}
	g_in_global = 1;
	rc = -global_lock_error();
	return rc ? finish(scope, rc) : 0;
}

static int lock_control(global_scope_t *scope)
{
	int rc = __cleanup_lockdep_mutex_lock(
		&g_control_mu, LOGGER_LOCK_GLOBAL_CONTROL);
	if (!rc) {
		scope->control_locked = 1;
		int poison = global_lock_error();
		if (poison)
			return -poison;
	}
	return -rc;
}

static int phase_error(uint64_t ticket)
{
	switch (phase_of(ticket)) {
	case G_IDLE:
		return ENODEV;
	case G_STARTING:
		return EAGAIN;
	case G_RUNNING:
		return 0;
	case G_BROKEN:
		return global_lock_error() ? global_lock_error() : EIO;
	default:
		return ESHUTDOWN;
	}
}

static int pin(global_scope_t *scope, int allow_bootstrap)
{
	uint64_t ticket = atomic_load_explicit(&g_ticket, memory_order_acquire);
	int error = phase_error(ticket);
	if (error && !(allow_bootstrap && phase_of(ticket) == G_IDLE))
		return -error;
	/* 所有读侧 API 都必须经过准入门禁，不只是 LOG_* 和 flush。
	 * 准入关闭后，持续的指标、设置或重开调用不能继续加入读写锁读者流。
	 * 已经延迟的调用最多进入一次，并在拿到生命周期保护后重新校验票据。
	 */
	int rc = __cleanup_lockdep_rwlock_rdlock(&g_lifetime_lock);
	if (rc)
		return -rc;
	scope->lifetime_locked = 1;
	int poison = global_lock_error();
	if (poison)
		return -poison;
	if (ticket != atomic_load_explicit(&g_ticket, memory_order_acquire))
		return -ESHUTDOWN;
	if (phase_of(ticket) == G_RUNNING && !g_logger)
		return -EIO; /* 内部不变量破坏，不允许降级为 stderr 回退 */
	return 0;
}

int logger_init(const logger_config_t *cfg)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* 重复初始化必须在创建文件、套接字或工作线程前快速拒绝，包括 cfg 非法的情况。
	 * 这样也避免连续重复初始化请求长期占用生命周期控制。
	 */
	if (phase_of(atomic_load_explicit(&g_ticket, memory_order_acquire)) ==
	    G_RUNNING)
		return finish(&scope, -EALREADY);
	int rc = lock_control(&scope);
	if (rc)
		return finish(&scope, rc);
	uint64_t old = atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (phase_of(old) == G_RUNNING)
		return finish(&scope, -EALREADY);
	/* 未完整退役的对象绝不能有后继对象，即使其他路径
	 * 错误处理了其代次票据也不例外。
	 */
	if (g_retired)
		return finish(&scope, -EUCLEAN);
	if (generation_of(old) == (UINT64_MAX >> PHASE_BITS))
		return finish(&scope,
			      -EOVERFLOW); /* generation 绝不回绕复用 */
	uint64_t ticket = ((generation_of(old) + 1u) << PHASE_BITS) |
			  G_STARTING;
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &old, ticket,
						     memory_order_acq_rel,
						     memory_order_acquire))
		return finish(&scope, -(global_lock_error() ?
					global_lock_error() : EIO));
	logger_t *candidate = NULL;
	rc = logger_create(cfg, &candidate);
	if (rc) {
		/* 初始引导构造失败时保留引导日志输出，但仍消费本次票据，
		 * 防止初始化前的延迟调用穿越到后续新实例。
		 */
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		rollback_ticket(ticket, rollback);
		return finish(&scope, rc);
	}
	rc = __cleanup_lockdep_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		(void)retire_candidate(candidate);
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		rollback_ticket(ticket, rollback);
		return finish(&scope, -rc);
	}
	scope.lifetime_locked = 1;
	/* 初始引导读者可能在构造期间使生命周期失效。
	 * 此候选对象从未发布，不能成为全局对象。
	 */
	int poison = global_lock_error();
	if (poison) {
		(void)retire_candidate(candidate);
		return finish(&scope, -poison);
	}
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME,
				   &g_lifetime_lock);
	g_logger = candidate;
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &ticket, with_phase(ticket, G_RUNNING),
		    memory_order_acq_rel, memory_order_acquire)) {
		/* 并发读者可能在先前检查后使生命周期失效。
		 * 尚未向任何获准读者发布：写侧生命周期保护仍被持有，
		 * 因此将此候选对象退役。
		 */
		g_logger = NULL;
		int error = global_lock_error();
		(void)retire_candidate(candidate);
		return finish(&scope, -(error ? error : EIO));
	}
	return finish(&scope, 0);
}

int logger_shutdown_status(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* 排队中的关闭操作绑定到这里观察到的 generation，
	 * 不能在等待其他控制方后转而关闭当时恰好存在的新实例。
	 */
	uint64_t observed =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	int rc = lock_control(&scope);
	if (rc)
		return finish(&scope, rc);
	uint64_t current =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (generation_of(observed) != generation_of(current))
		return finish(&scope, -ESHUTDOWN);
	if (phase_of(current) == G_STOPPED)
		return finish(
			&scope,
			0); /* 停止已完成，不代表已释放对象可以被重试使用 */
	if (phase_of(current) != G_RUNNING && phase_of(current) != G_IDLE)
		return finish(
			&scope,
			-EIO); /* STARTING/STOPPING 完全由生命周期控制方拥有 */
	/* IDLE 状态仍可能存在受生命周期保护的初始引导 stderr 写者。
	 * 即使没有 logger 对象，也必须先关闭准入并等待该读者离开。
	 */
	uint64_t stopping = with_phase(current, G_STOPPING);
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &current, stopping, memory_order_acq_rel,
		    memory_order_acquire))
		return finish(&scope, -(global_lock_error() ?
					global_lock_error() : EIO));
	rc = __cleanup_lockdep_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		/* 获取失败时，只有在等待已有读者期间无人使本代失效，
		 * 才重新开放准入。
		 */
		(void)atomic_compare_exchange_strong_explicit(
			&g_ticket, &stopping, current,
			memory_order_release, memory_order_relaxed);
		return finish(&scope, -rc);
	}
	/* 读者可能在我们获取写锁之前发现无效解锁。 */
	scope.lifetime_locked = 1;
	int poison = global_lock_error();
	if (poison)
		return finish(&scope, -poison);
	scope.lifetime_locked = 0; /* 成功路径显式解锁 */
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME,
				   &g_lifetime_lock);
	logger_t *old = g_logger;
	/* 在写锁下撤销发布；控制方保留唯一所有者，
	 * 直到证明最后一个拆除资源已释放。
	 */
	g_retired = old;
	g_logger = NULL;
	int unlock_rc = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
	if (unlock_rc) {
		/* 生命周期所有权不确定：保留旧分配和外层控制锁。
		 * STOPPED 和最终 free 都不合法。
		 */
		global_lock_poison(unlock_rc);
		scope.proof_error = unlock_rc;
		return finish(&scope, -unlock_rc);
	}
	/* 写锁释放证明外部读者已排空。内部回执区分
	 * 最终 free 与工作线程拆除未完成。
	 */
	logger_destroy_receipt_reset();
	rc = logger_destroy(&old);
	if (!logger_destroy_receipt_read())
		global_lock_poison(rc < 0 ? -rc : EUCLEAN);
	else {
		g_retired = NULL;
		/* 较晚发生的读者释放失败不能被 STOPPED 抹除。
		 * 后端 I/O 失败时仍可能在完成最终 free 后返回。
		 */
		if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &stopping, with_phase(current, G_STOPPED),
			    memory_order_acq_rel, memory_order_acquire)) {
			int error = global_lock_error();
			if (!error) {
				error = EUCLEAN;
				global_lock_poison(error);
			}
			if (!rc)
				rc = -error;
		}
	}
	return finish(&scope, rc);
}

void logger_shutdown(void)
{
	(void)logger_shutdown_status();
}

int logger_flush_status(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = pin(&scope, 0);
	if (!rc && logger_flush_instance_status(g_logger))
		rc = -(errno ? errno : EIO);
	return finish(&scope, rc);
}

void logger_flush(void)
{
	(void)logger_flush_status();
}

void logger_set_level(logger_level_t level)
{
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = (unsigned)level <= LOGGER_OFF ? pin(&scope, 0) : -EINVAL;
	if (!rc)
		logger_set_instance_level(g_logger, level);
	(void)finish(&scope, rc);
}

uint64_t logger_global_dropped(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return 0;
	int rc = pin(&scope, 0);
	uint64_t dropped = rc ? 0 : logger_dropped(g_logger);
	(void)finish(&scope, rc);
	return dropped;
}

int logger_reopen(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = pin(&scope, 0);
	if (!rc && logger_reopen_instance(g_logger))
		rc = -(errno ? errno : EIO);
	return finish(&scope, rc);
}

void logger_get_global_metrics(logger_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = pin(&scope, 0);
	if (!rc)
		logger_get_metrics(g_logger, out);
	(void)finish(&scope, rc);
}

void logger_get_global_io_metrics(logger_io_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = pin(&scope, 0);
	if (!rc)
		logger_get_io_metrics(g_logger, out);
	(void)finish(&scope, rc);
}

void logger_get_global_diagnostics(logger_diagnostics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = pin(&scope, 0);
	if (!rc)
		logger_get_diagnostics(g_logger, out);
	(void)finish(&scope, rc);
}

int logger_get_global_file_metrics(logger_file_metrics_t *out)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = !out ? -EINVAL : pin(&scope, 0);
	if (!rc)
		rc = logger_file_metrics_snapshot(g_logger, out);
	return finish(&scope, rc);
}

void logger_global_write(logger_level_t level, const char *module,
			 const char *file, int line, const char *func,
			 const char *fmt, ...)
{
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL :
							 pin(&scope, 1);
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		if (g_logger) {
			logger_vlog_internal(g_logger, level, module, file,
					     line, func, fmt, ap);
		} else {
			/* 仅供应用使用的旧有初始引导输出，只存在于首次成功初始化或显式关闭前。
			 * 生命周期保护覆盖整个 I/O，因此旧的延迟消息不能在新实例发布后继续输出。
			 */
			char text[LOGGER_MESSAGE_MAX];
			int n = logger_libc_vsnprintf(text, sizeof(text), fmt,
						      ap);
			if (n < 0)
				rc = -EILSEQ;
			else {
				if ((size_t)n >= sizeof(text))
					logger_text_mark_truncated(text, sizeof(text));
				/* 初始引导诊断需要与已初始化的 stderr 输出端相同的线程局部 SIGPIPE 保护。
				 * 保留历史字节，不再进行第二次无界 printf 分配。
				 */
				char name[6];
				(void)logger_libc_snprintf(
					name, sizeof(name), "%-5s",
					logger_level_name(level));
				const char *tag = module ? module : "app";
				struct iovec vec[] = {
					{ .iov_base = name, .iov_len = 5 },
					{ .iov_base = " [", .iov_len = 2 },
					{ .iov_base = (void *)tag, .iov_len = strlen(tag) },
					{ .iov_base = "] ", .iov_len = 2 },
					{ .iov_base = text, .iov_len = strlen(text) },
					{ .iov_base = "\n", .iov_len = 1 }
				};
				rc = logger_stderr_writev_all(vec,
						(int)(sizeof(vec) / sizeof(vec[0])));
			}
		}
		va_end(ap);
	}
	(void)finish(&scope, rc);
}

int logger_global_scope_busy(void)
{
	return g_in_global;
}

void logger_global_reset_child(void)
{
	g_in_global = 0;
}
