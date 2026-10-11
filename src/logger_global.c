#include "logger_internal.h"
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

/* 只有 lifecycle control 操作获取 g_control_mu。
 * 普通调用通过 g_lifetime_lock 的 read-side pin 保证对象存活。
 * 这个 rwlock pin 不是 refcount：reader 只在持有 read lock 时借用 g_logger。
 * 锁顺序固定为 control -> lifetime -> instance locks。
 * 持有 lifetime read lock 时禁止反向获取 control。
 */
static pthread_mutex_t g_control_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_lifetime_lock = PTHREAD_RWLOCK_INITIALIZER;
static logger_t *g_logger; /* 由 lifetime lock 保护；绝不把 owning pointer 返回给应用 */
/* Controller-owned unpublished/retired instance, protected by g_control_mu.
 * A failed final-release proof deliberately retains the allocation here.
 * This is a real owner, not a sanitizer-only leak suppression or a refcount. */
static logger_t *g_retired;

enum global_phase {
	G_IDLE, G_STARTING, G_RUNNING, G_STOPPING, G_STOPPED, G_BROKEN
};
#define PHASE_BITS 3u
#define PHASE_MASK UINT64_C(7)
/* generation 与 phase 放在同一个 atomic ticket 中。
 * 若分开读取，reader 可能把某一代 RUNNING 与另一代 generation 错配。
 * 每次真实 init 尝试都会消费一个 generation，即使 candidate 构造失败。
 */
static _Atomic uint64_t g_ticket;
/* A failed lock release or incomplete teardown invalidates global lifetime. */
static _Atomic int g_lock_error;
static _Thread_local int g_in_global;

typedef struct {
	int old_cancel;
	int saved_errno;
	int control_locked;
	int lifetime_locked;
	int proof_error; /* current thread may still hold an unverified lock */
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

/* Publish the sticky error before the broken phase. A CAS preserves a newer
 * generation if another controller was already waiting on the lock. */
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

/* An init rollback must never undo another thread's proof failure. */
static void rollback_ticket(uint64_t ticket, enum global_phase phase)
{
	(void)atomic_compare_exchange_strong_explicit(
		&g_ticket, &ticket, with_phase(ticket, phase),
		memory_order_release, memory_order_relaxed);
}

/* Called with Global control ownership and an unpublished candidate.
 * The same receipt also covers finalization errors that accompany a
 * successful free; only actual incomplete release retains this owner. */
static int retire_candidate(logger_t *candidate)
{
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_CONTROL,
				   &g_control_mu);
	g_retired = candidate;
	logger_destroy_receipt_reset();
	int rc = logger_destroy_status(candidate) ? -(errno ? errno : EIO) : 0;
	if (logger_destroy_receipt_read())
		g_retired = NULL;
	else
		global_lock_poison(rc < 0 ? -rc : EUCLEAN);
	return rc;
}

/* cancellation 恢复生效前，必须先释放全部资源和 lifetime pin。
 * 应用的 cancellation cleanup handler 随后仍可能调用 Logger。
 * 这里是 cancellation deferral，不是 rollback，也不是 I/O deadline。
 * explicit-instance API 还会建立自己的 inner scope，并继承 disabled 状态。
 */
static int finish(global_scope_t *scope, int rc)
{
	int error;
	if (scope->proof_error) {
		/* A manual unlock already failed. Retain outer locks and keep
		 * cancellation disabled; repeating unlock cannot prove ownership. */
		errno = scope->proof_error;
		return -1;
	}
	if (scope->lifetime_locked) {
		error = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
		if (error) {
			global_lock_poison(error);
			/* The lifetime pin may still be held. Do not unlock the
			 * controller, clear TLS reentry, or resume cancellation. */
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
	int poison = global_lock_error();
	if (poison) {
		errno = poison;
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
	/* 所有 read-side API 都必须经过 admission gate，不只是 LOG_* 和 flush。
	 * admission 关闭后，持续的 metrics/set/reopen 调用不能继续加入 rwlock reader 流。
	 * 已经延迟的调用最多进入一次，并在拿到 lifetime pin 后重新校验 ticket。
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
	/* An incompletely retired object must never have a successor, even if
	 * its generation ticket were to be mishandled by another path. */
	if (g_retired)
		return finish(&scope, -EUCLEAN);
	if (generation_of(old) == (UINT64_MAX >> PHASE_BITS))
		return finish(&scope,
			      -EOVERFLOW); /* generation 绝不回绕复用 */
	uint64_t ticket = ((generation_of(old) + 1u) << PHASE_BITS) |
			  G_STARTING;
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &old, ticket,
		    memory_order_release, memory_order_acquire))
		return finish(&scope, -(global_lock_error() ?
					global_lock_error() : EIO));
	logger_t *candidate = logger_create(cfg);
	if (!candidate) {
		rc = -(errno ? errno : EIO);
		/* bootstrap 构造失败时保留 bootstrap logging，但仍消费本次 ticket，
		 * 防止 pre-init 延迟调用穿越到后续新实例。
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
	/* A bootstrap reader can poison the lifetime during construction.
	 * This candidate was never published and must not become global. */
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
		    memory_order_release, memory_order_acquire)) {
		/* A concurrent reader may have poisoned the lifetime after the
		 * earlier check. Nothing was published to an admitted reader:
		 * the writer pin is still held, so retire this candidate. */
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
	uint64_t stopping = with_phase(current, G_STOPPING);
	if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &current, stopping,
		    memory_order_release, memory_order_acquire))
		return finish(&scope, -(global_lock_error() ?
					global_lock_error() : EIO));
	rc = __cleanup_lockdep_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		/* On failed acquire, reopen admission only if nobody poisoned this
		 * generation while waiting for preexisting readers. */
		(void)atomic_compare_exchange_strong_explicit(
			&g_ticket, &stopping, current,
			memory_order_release, memory_order_relaxed);
		return finish(&scope, -rc);
	}
	/* A reader may discover an invalid unlock before we acquire writer. */
	scope.lifetime_locked = 1;
	int poison = global_lock_error();
	if (poison)
		return finish(&scope, -poison);
	scope.lifetime_locked = 0; /* successful path unlocks explicitly */
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME,
				   &g_lifetime_lock);
	logger_t *old = g_logger;
	/* Unpublish under writer lock; the controller retains the only owner
	 * until the last teardown resource has been proven released. */
	g_retired = old;
	g_logger = NULL;
	int unlock_rc = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
	if (unlock_rc) {
		/* Lifetime ownership is uncertain: retain the old allocation and
		 * the outer control lock. Neither STOPPED nor final free is legal. */
		global_lock_poison(unlock_rc);
		scope.proof_error = unlock_rc;
		return finish(&scope, -unlock_rc);
	}
	/* Writer lock release proves external readers drained. The internal
	 * receipt distinguishes final free from an incomplete worker teardown. */
	logger_destroy_receipt_reset();
	rc = logger_destroy_status(old) ? -(errno ? errno : EIO) : 0;
	if (!logger_destroy_receipt_read())
		global_lock_poison(rc < 0 ? -rc : EUCLEAN);
	else {
		g_retired = NULL;
		/* A late reader release failure must not be erased by STOPPED.
		 * Backend I/O failure may still return with final free complete. */
		if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &stopping, with_phase(current, G_STOPPED),
		    memory_order_release, memory_order_acquire)) {
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
			/* 仅供应用使用的 legacy bootstrap 输出，只存在于首次成功初始化/显式 shutdown 前。
			 * lifetime pin 覆盖整个 I/O，因此旧的延迟消息不能在新实例 publish 后继续输出。
			 */
			char text[LOGGER_MESSAGE_MAX];
			int n = vsnprintf(text, sizeof(text), fmt, ap);
			if (n < 0)
				rc = -EILSEQ;
			else {
				if ((size_t)n >= sizeof(text))
					logger_text_mark_truncated(text, sizeof(text));
				/* Bootstrap diagnostics need the same thread-local SIGPIPE
				 * protection as initialized stderr sinks. Keep the historical
				 * bytes without a second unbounded printf allocation. */
				char name[6];
				(void)snprintf(name, sizeof(name), "%-5s",
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

#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_global_lifetime_broken(void)
{
	return global_lock_error() != 0;
}

int logger_global_stop_for_clean_fork(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -errno;
	int rc = __cleanup_lockdep_mutex_trylock(
		&g_control_mu, LOGGER_LOCK_GLOBAL_CONTROL);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.control_locked = 1;
	rc = __cleanup_lockdep_rwlock_trywrlock(&g_lifetime_lock);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.lifetime_locked = 1;
	int poison = global_lock_error();
	if (poison) {
		(void)finish(&scope, -poison);
		return -poison;
	}
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
		if (!atomic_compare_exchange_strong_explicit(
		    &g_ticket, &current, with_phase(current, G_STOPPING),
		    memory_order_release, memory_order_acquire)) {
			rc = -(global_lock_error() ? global_lock_error() : EIO);
			goto out;
		}
		g_retired = l;
		g_logger = NULL;
		int unlock_rc = __cleanup_lockdep_rwlock_unlock(&g_lifetime_lock);
		scope.lifetime_locked = 0;
		if (unlock_rc) {
			global_lock_poison(unlock_rc);
			scope.proof_error = unlock_rc;
			rc = -unlock_rc;
		} else {
			logger_destroy_receipt_reset();
			rc = logger_destroy_status(l) ? -(errno ? errno : EIO) : 0;
			if (!logger_destroy_receipt_read())
				global_lock_poison(rc < 0 ? -rc : EUCLEAN);
			else {
				g_retired = NULL;
				uint64_t stopping = with_phase(current, G_STOPPING);
				if (!atomic_compare_exchange_strong_explicit(
				    &g_ticket, &stopping, with_phase(current, G_STOPPED),
				    memory_order_release, memory_order_acquire)) {
					int error = global_lock_error();
					if (!error) {
						error = EUCLEAN;
						global_lock_poison(error);
					}
					if (!rc)
						rc = -error;
				}
			}
		}
	}
out:
	if (finish(&scope, rc) && !rc)
		return -(errno ? errno : EIO);
	return rc;
}
#endif
