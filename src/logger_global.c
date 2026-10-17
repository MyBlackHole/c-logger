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

enum global_phase { G_IDLE, G_RUNNING, G_STOPPING, G_BROKEN };
#define PHASE_BITS 3u
#define PHASE_MASK UINT64_C(7)
/* generation 与 phase 放在同一个原子票据中。
 * 若分开读取，读者可能把某一代 RUNNING 与另一代 generation 错配。
 * 每次真正改变默认绑定都会消费一个 generation，不会创建或销毁实例。
 *
 * CAS 失败时需要 acquire 的生命周期转换，成功时也保留 acquire。
 * acq_rel/acquire 保留 release 发布，并明确满足本项目采用的 C11 约定，
 * 不依赖 release/acquire 在历史“失败序不得更强”措辞下的歧义。
 * 票据只控制准入，对象存活仍由 g_lifetime_lock 的生命周期锁保证。
 */
#define g_ticket (logger_runtime_current()->global_ticket)
/* 借用锁释放证明失败会使默认绑定失效，不能据此释放任何宿主对象。 */
#define g_lock_error (logger_runtime_current()->global_lock_error)
static _Thread_local int g_in_global;

typedef struct {
	int old_cancel;
	int saved_errno;
	int control_locked;
	int lifetime_locked;
	logger_t *release_borrow; /* 所有解锁已确认后，才撤销旧实例的借用标记 */
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

/* 绑定回滚绝不能撤销另一线程的证明失败。 */
static void rollback_ticket(uint64_t ticket, enum global_phase phase)
{
	(void)atomic_compare_exchange_strong_explicit(
		&g_ticket, &ticket, with_phase(ticket, phase),
		memory_order_release, memory_order_relaxed);
}

/* 取消恢复生效前，必须先释放全部资源和生命周期保护。
 * 应用的取消清理处理函数随后仍可能调用 Logger。
 * 这里是取消延后，不是回滚，也不是 I/O 超时期限。
 * 显式实例 API 还会建立自己的内层作用域，并继承禁用状态。
 */
static int finish_result(global_scope_t *scope, int rc)
{
	int error;

	if (scope->lifetime_locked) {
		error = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
		if (error) {
			global_lock_poison(error);
			return -error;
		}
	}
	if (scope->control_locked) {
		error = __cleanup_lockdep_mutex_unlock(
			&g_control_mu, LOGGER_LOCK_GLOBAL_CONTROL);
		if (error) {
			global_lock_poison(error);
			return -error;
		}
	}
	/* 清除标记是宿主可以销毁旧实例的最后一道凭证，早于取消恢复。
	 * 任何解锁证明失败都不清标记，不以指针已移出槽推导借用已归还。
	 */
	if (scope->release_borrow)
		atomic_store_explicit(&scope->release_borrow->default_borrowed,
				      0, memory_order_release);
	g_in_global = 0;
	error = pthread_setcancelstate(scope->old_cancel, NULL);
	return rc ? rc : -error;
}

/* 级别设置及 begin 的内部错误桥接仍使用 POSIX 风格。 */
static int finish(global_scope_t *scope, int rc)
{
	rc = finish_result(scope, rc);
	errno = rc < 0 ? -rc : scope->saved_errno;
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
	case G_STOPPING:
		return EAGAIN;
	case G_RUNNING:
		return 0;
	case G_BROKEN:
		return global_lock_error() ? global_lock_error() : EIO;
	default:
		return ENODEV;
	}
}

static int pin(global_scope_t *scope)
{
	uint64_t ticket = atomic_load_explicit(&g_ticket, memory_order_acquire);
	int error = phase_error(ticket);
	if (error)
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
		return -EIO; /* 内部不变量破坏，不能对空指针取得借用 */
	return 0;
}

int logger_default_set(logger_t *expected, logger_t *replacement)
{
	int saved = errno;
	global_scope_t scope;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto out;
	}
	/* 控制请求也固定进入时的票据，排队后不能改变新一代绑定。
	 * 同一实例经历解绑、重绑形成的指针 ABA 不会绕过代次复核。
	 */
	uint64_t observed =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (phase_of(observed) == G_STOPPING) {
		rc = -EAGAIN;
		goto done;
	}
	rc = lock_control(&scope);
	if (rc)
		goto done;
	uint64_t current =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (current != observed || g_logger != expected) {
		rc = -ESTALE;
		goto done;
	}
	if (expected == replacement) {
		rc = 0;
		goto done;
	}
	if (replacement &&
	    atomic_load_explicit(&replacement->state, memory_order_acquire) !=
		    LOGGER_STATE_RUNNING) {
		rc = -ESHUTDOWN;
		goto done;
	}
	if (replacement && atomic_load_explicit(&replacement->default_borrowed,
						memory_order_acquire)) {
		rc = -EBUSY;
		goto done;
	}
	if (generation_of(current) == (UINT64_MAX >> PHASE_BITS)) {
		rc = -EOVERFLOW;
		goto done;
	}
	uint64_t updating = with_phase(current, G_STOPPING);
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &current, updating, memory_order_acq_rel,
		    memory_order_acquire)) {
		rc = -(global_lock_error() ? global_lock_error() : EIO);
		goto done;
	}
	/* 关闭新读者准入后，再等待已经取得借用的调用退出。 */
	rc = __cleanup_lockdep_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		rollback_ticket(updating, phase_of(current));
		rc = -rc;
		goto done;
	}
	scope.lifetime_locked = 1;
	rc = -global_lock_error();
	if (rc)
		goto done;
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME,
				   &g_lifetime_lock);
	if (replacement)
		atomic_store_explicit(&replacement->default_borrowed, 1,
				      memory_order_release);
	g_logger = replacement;
	uint64_t next = ((generation_of(current) + 1u) << PHASE_BITS) |
			(replacement ? G_RUNNING : G_IDLE);
	if (!atomic_compare_exchange_strong_explicit(&g_ticket, &updating, next,
						     memory_order_acq_rel,
						     memory_order_acquire)) {
		/* 发布前被延迟读者毒化。无人获准使用 replacement，恢复旧槽；
		 * 只有后续解锁全部确认才归还 replacement 的临时借用。
		 */
		g_logger = expected;
		scope.release_borrow = replacement;
		rc = -(global_lock_error() ? global_lock_error() : EIO);
	} else {
		scope.release_borrow = expected;
		rc = 0;
	}
done:
	rc = finish_result(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_default_flush(void)
{
	int saved = errno;
	global_scope_t scope;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto out;
	}
	rc = pin(&scope);
	if (!rc)
		rc = logger_flush(g_logger);
	rc = finish_result(&scope, rc);
out:
	errno = saved;
	return rc;
}

void logger_set_level(logger_level_t level)
{
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = (unsigned)level <= LOGGER_OFF ? pin(&scope) : -EINVAL;
	if (!rc)
		logger_set_instance_level(g_logger, level);
	(void)finish(&scope, rc);
}

int logger_global_dropped(uint64_t *out)
{
	int saved = errno;
	global_scope_t scope;
	uint64_t snapshot;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto done;
	}
	rc = !out ? -EINVAL : pin(&scope);
	if (!rc)
		rc = logger_dropped(g_logger, &snapshot);
	rc = finish_result(&scope, rc);
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_default_reopen(void)
{
	int saved = errno;
	global_scope_t scope;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto out;
	}
	rc = pin(&scope);
	if (!rc)
		rc = logger_reopen(g_logger);
	rc = finish_result(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_get_global_metrics(logger_metrics_t *out)
{
	int saved = errno;
	global_scope_t scope;
	logger_metrics_t snapshot;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto done;
	}
	rc = !out ? -EINVAL : pin(&scope);
	if (!rc)
		rc = logger_get_metrics(g_logger, &snapshot);
	rc = finish_result(&scope, rc);
	/* 借用释放和取消恢复都成功后，才写出不再依赖实例寿命的副本。 */
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_get_global_io_metrics(logger_io_metrics_t *out)
{
	int saved = errno;
	global_scope_t scope;
	logger_io_metrics_t snapshot;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto done;
	}
	rc = !out ? -EINVAL : pin(&scope);
	if (!rc)
		rc = logger_get_io_metrics(g_logger, &snapshot);
	rc = finish_result(&scope, rc);
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_get_global_diagnostics(logger_diagnostics_t *out)
{
	int saved = errno;
	global_scope_t scope;
	logger_diagnostics_t snapshot;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto done;
	}
	rc = !out ? -EINVAL : pin(&scope);
	if (!rc)
		rc = logger_get_diagnostics(g_logger, &snapshot);
	rc = finish_result(&scope, rc);
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_get_global_file_metrics(logger_file_metrics_t *out)
{
	int saved = errno;
	global_scope_t scope;
	logger_file_metrics_t snapshot;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto done;
	}
	rc = !out ? -EINVAL : pin(&scope);
	if (!rc)
		rc = logger_file_metrics_snapshot(g_logger, &snapshot);
	rc = finish_result(&scope, rc);
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_global_write(logger_level_t level, const char *module,
			const char *file, int line, const char *func,
			const char *fmt, ...)
{
	int saved = errno;
	global_scope_t scope;
	int rc;

	if (begin(&scope)) {
		rc = -errno;
		goto out;
	}
	rc = !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL : pin(&scope);
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		/* 默认借用的成功锁操作也可能改变 errno；%m 使用最外层入口值。 */
		errno = saved;
		rc = logger_vlog_internal(g_logger, level, module, file, line,
					  func, fmt, ap);
		va_end(ap);
	}
	rc = finish_result(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_global_scope_busy(void)
{
	return g_in_global;
}

void logger_global_reset_child(void)
{
	g_in_global = 0;
}
