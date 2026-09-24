#include "logger_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 只有 lifecycle control 操作获取 g_control_mu。
 * 普通调用通过 g_lifetime_lock 的 read-side pin 保证对象存活。
 * 这个 rwlock pin 不是 refcount：reader 只在持有 read lock 时借用 g_logger。
 * 锁顺序固定为 control -> lifetime -> instance locks。
 * 持有 lifetime read lock 时禁止反向获取 control。
 */
static pthread_mutex_t g_control_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_lifetime_lock = PTHREAD_RWLOCK_INITIALIZER;
static logger_t *g_logger; /* 由 lifetime lock 保护；绝不把 owning pointer 返回给应用 */

enum global_phase { G_IDLE, G_STARTING, G_RUNNING, G_STOPPING, G_STOPPED };
#define PHASE_BITS 3u
#define PHASE_MASK UINT64_C(7)
/* generation 与 phase 放在同一个 atomic ticket 中。
 * 若分开读取，reader 可能把某一代 RUNNING 与另一代 generation 错配。
 * 每次真实 init 尝试都会消费一个 generation，即使 candidate 构造失败。
 */
static _Atomic uint64_t g_ticket;
static _Thread_local int g_in_global;
static _Thread_local int g_fork_prepared;

typedef struct {
	int old_cancel;
	int saved_errno;
	int control_locked;
	int lifetime_locked;
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

/* cancellation 恢复生效前，必须先释放全部资源和 lifetime pin。
 * 应用的 cancellation cleanup handler 随后仍可能调用 Logger。
 * 这里是 cancellation deferral，不是 rollback，也不是 I/O deadline。
 * explicit-instance API 还会建立自己的 inner scope，并继承 disabled 状态。
 */
static int finish(global_scope_t *scope, int rc)
{
	int error;
	if (scope->lifetime_locked) {
		error = pthread_rwlock_unlock(&g_lifetime_lock);
		if (!rc && error)
			rc = -error;
	}
	if (scope->control_locked) {
		error = pthread_mutex_unlock(&g_control_mu);
		if (!rc && error)
			rc = -error;
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
	/* 接触继承来的同步对象或 once state 前，先检查 process identity。 */
	if (logger_process_is_child()) {
		errno = ECHILD;
		return -1;
	}
	/* 拒绝同线程直接重入，例如 printf/interposition callback 回调 Logger。
	 * 这并不意味着 signal handler 或跨线程 callback 环是安全的，
	 * 也不允许 pthread_exit/longjmp 穿过 Logger 调用。
	 */
	if (g_in_global || logger_scope_busy()) {
		errno = EDEADLK;
		return -1;
	}
	*scope = (global_scope_t){ .saved_errno = errno };
	int rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					&scope->old_cancel);
	if (rc) {
		errno = rc;
		return -1;
	}
	g_in_global = 1;
	rc = logger_process_ensure();
	return rc ? finish(scope, rc) : 0;
}

static int lock_control(global_scope_t *scope)
{
	int rc = pthread_mutex_lock(&g_control_mu);
	if (!rc)
		scope->control_locked = 1;
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
	/* 所有 read-side API 都必须经过 admission gate，不只是 LOG_* 和 flush。
	 * admission 关闭后，持续的 metrics/set/reopen 调用不能继续加入 rwlock reader 流。
	 * 已经延迟的调用最多进入一次，并在拿到 lifetime pin 后重新校验 ticket。
	 */
	int rc = pthread_rwlock_rdlock(&g_lifetime_lock);
	if (rc)
		return -rc;
	scope->lifetime_locked = 1;
	if (ticket != atomic_load_explicit(&g_ticket, memory_order_acquire))
		return -ESHUTDOWN;
	if (phase_of(ticket) == G_RUNNING && !g_logger)
		return -EIO; /* 内部 invariant 破坏，不允许降级为 stderr fallback */
	return 0;
}

int logger_init(const logger_config_t *cfg)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* repeat init 必须在创建 file/socket/worker 前快速拒绝，包括 cfg 非法的情况。
	 * 这样也避免连续 repeat-init 请求长期占用 lifecycle control。
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
	if (generation_of(old) == (UINT64_MAX >> PHASE_BITS))
		return finish(&scope,
			      -EOVERFLOW); /* generation 绝不回绕复用 */
	uint64_t ticket = ((generation_of(old) + 1u) << PHASE_BITS) |
			  G_STARTING;
	atomic_store_explicit(&g_ticket, ticket, memory_order_release);
	logger_t *candidate = logger_create(cfg);
	if (!candidate) {
		rc = -(errno ? errno : EIO);
		/* bootstrap 构造失败时保留 bootstrap logging，但仍消费本次 ticket，
		 * 防止 pre-init 延迟调用穿越到后续新实例。
		 */
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		atomic_store_explicit(&g_ticket, with_phase(ticket, rollback),
				      memory_order_release);
		return finish(&scope, rc);
	}
	rc = pthread_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		(void)logger_destroy_status(candidate);
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		atomic_store_explicit(&g_ticket, with_phase(ticket, rollback),
				      memory_order_release);
		return finish(&scope, -rc);
	}
	scope.lifetime_locked = 1;
	g_logger = candidate;
	atomic_store_explicit(&g_ticket, with_phase(ticket, G_RUNNING),
			      memory_order_release);
	return finish(&scope, 0);
}

int logger_shutdown_status(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* 排队中的 shutdown 绑定到这里观察到的 generation，
	 * 不能在等待其他 controller 后转而关闭当时恰好存在的新实例。
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
			0); /* stop 已完成，不代表已释放对象可以被重试使用 */
	if (phase_of(current) != G_RUNNING && phase_of(current) != G_IDLE)
		return finish(
			&scope,
			-EIO); /* STARTING/STOPPING 完全由 lifecycle controller 拥有 */
	/* IDLE 状态仍可能存在被 pin 的 bootstrap stderr writer。
	 * 即使没有 logger 对象，也必须先关闭 admission 并等待该 reader 离开。 */
	atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPING),
			      memory_order_release);
	rc = pthread_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		/* 尚未修改 pointer/resource，因此 acquisition 失败时可以重新开放 admission。 */
		atomic_store_explicit(&g_ticket, current, memory_order_release);
		return finish(&scope, -rc);
	}
	logger_t *old = g_logger;
	g_logger = NULL;
	pthread_rwlock_unlock(&g_lifetime_lock);
	/* 整个 I/O、worker join、close 和 owner release 期间持续持有 control。 */
	rc = logger_destroy_status(old) ? -(errno ? errno : EIO) : 0;
	atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPED),
			      memory_order_release);
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
			/* 仅供应用使用的 legacy bootstrap 输出，只存在于首次成功初始化/显式 shutdown 前。
			 * lifetime pin 覆盖整个 I/O，因此旧的延迟消息不能在新实例 publish 后继续输出。
			 */
			char text[LOGGER_MESSAGE_MAX];
			int n = vsnprintf(text, sizeof(text), fmt, ap);
			if (n < 0)
				rc = -EILSEQ;
			else if (dprintf(STDERR_FILENO, "%-5s [%s] %s\n",
					 logger_level_name(level),
					 module ? module : "app", text) < 0)
				rc = -(errno ? errno : EIO);
		}
		va_end(ap);
	}
	(void)finish(&scope, rc);
}

int logger_prepare_fork(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = g_fork_prepared ? -EALREADY : 0;
	if (!rc)
		g_fork_prepared = 1;
	/* 这里只记录 marker；不持锁、不停止 worker、不 flush queue，也不提供 fork 安全保证。 */
	return finish(&scope, rc);
}

void logger_after_fork_parent(void)
{
	if (logger_process_is_child()) {
		errno = ECHILD;
		return;
	}
	g_fork_prepared = 0;
}

void logger_after_fork_child(void)
{
	logger_process_invalidate_child();
}

#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_global_stop_for_clean_fork(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -errno;
	int rc = pthread_mutex_trylock(&g_control_mu);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.control_locked = 1;
	rc = pthread_rwlock_trywrlock(&g_lifetime_lock);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.lifetime_locked = 1;
	logger_t *l = g_logger;
	unsigned expected_objects = l ? 1u : 0u;
	unsigned expected_threads = 1u + (l && l->async_mode ? 1u : 0u);
	unsigned threads = 0;
	if (l && l->async_mode && pthread_equal(pthread_self(), l->worker))
		rc = -EDEADLK;
	else if (logger_process_object_count() != expected_objects)
		rc = -EBUSY;
	else {
		rc = logger_process_thread_count(&threads);
		if (!rc && threads != expected_threads)
			rc = -EBUSY;
	}
	if (!rc) {
		uint64_t current =
			atomic_load_explicit(&g_ticket, memory_order_acquire);
		atomic_store_explicit(&g_ticket,
				      with_phase(current, G_STOPPING),
				      memory_order_release);
		g_logger = NULL;
		pthread_rwlock_unlock(&g_lifetime_lock);
		scope.lifetime_locked = 0;
		rc = logger_dispose_internal(l);
		atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPED),
				      memory_order_release);
	}
	(void)finish(&scope, rc);
	return rc;
}
#endif
