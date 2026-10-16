#include "logger.h"
#include "logger_process.h"
#include "logger_fork_fd.h"
#include "logger_runtime.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* PID 与初始化票据在子进程拒绝路径中也要使用，不能落入 libatomic 锁。 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "fork guard requires lock-free int");
static _Atomic int owner_pid;
static _Atomic int forked_child;
/* 负数表示阶段，正数完整保存最近一次注册错误，不额外发布普通错误字段。 */
enum process_phase {
	PROCESS_IDLE = 0,
	PROCESS_STARTING = -1,
	PROCESS_READY = -2
};
static _Atomic int process_ticket;
static _Thread_local int initializing;
/* 这不是实例引用计数，也不是全部在途操作/继承资源的清单。 */
/* 实例计数属于新进程域；不清零继承的旧对象计数。 */
#define live_objects (logger_runtime_current()->live_objects)
static _Atomic int handlers_registered;

void logger_process_invalidate_child(void)
{
	/* 先关闭普通准入，再消费描述符快照；尚不重建继承的锁/队列。 */
	atomic_store_explicit(&forked_child, 1, memory_order_relaxed);
	logger_fork_fd_child();
	logger_runtime_invalidate_child();
}

int logger_process_is_child(void)
{
	if (atomic_load_explicit(&forked_child, memory_order_relaxed))
		return 1;
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	/* 覆盖注册尚未完成及宿主更早注册的 child handler。 */
	return owner != 0 && owner != (int)getpid();
}

static int claim_process(void)
{
	if (logger_process_is_child())
		return -ECHILD;
	int current = (int)getpid();
	int expected = 0;

	/* 注册前就发布 PID。子方不得等待消失线程的 STARTING 票据。 */
	(void)atomic_compare_exchange_strong_explicit(&owner_pid, &expected,
						      current,
						      memory_order_release,
						      memory_order_relaxed);
	return expected != 0 && expected != current ? -ECHILD : 0;
}

static int initialize_process(void)
{
	int child = logger_process_is_child();
	if (child &&
	    (!atomic_load_explicit(&forked_child, memory_order_relaxed) ||
	     !logger_fork_fd_child_valid()))
		return -ECHILD;
	int rc = child ? 0 : claim_process();
	if (rc)
		return rc;
	if (initializing)
		return -EDEADLK;
	int ticket;

	ticket = atomic_load_explicit(&process_ticket, memory_order_acquire);
	if (!child && ticket == PROCESS_READY)
		return -EALREADY;
	if (ticket == PROCESS_STARTING)
		return -EAGAIN;

	/* 在取得初始化所有权前禁止取消，避免票据永久停在 STARTING。
	 * 完成发布后恢复调用方策略；待定取消不会撤销已注册的进程能力。
	 */
	int old_cancel;
	rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel);
	if (rc)
		return -rc;
	if (!atomic_compare_exchange_strong_explicit(
		    &process_ticket, &ticket, PROCESS_STARTING,
		    memory_order_acq_rel, memory_order_acquire)) {
		rc = ticket == PROCESS_READY ? -EALREADY : -EAGAIN;
	} else {
		initializing = 1;
		struct logger_runtime *runtime = NULL;
		int result = child ? logger_fork_fd_child_reinit() : 0;
		if (!result)
			result = logger_runtime_create(&runtime);
		int error = result < 0 ? -result : 0;
		if (!error && !atomic_load_explicit(&handlers_registered,
						    memory_order_acquire)) {
			error = pthread_atfork(logger_fork_fd_prepare,
					       logger_fork_fd_parent,
					       logger_process_invalidate_child);
			if (!error)
				atomic_store_explicit(&handlers_registered, 1,
						      memory_order_release);
		}
		if (error) {
			(void)logger_runtime_discard(runtime);
		} else {
			if (child) {
				logger_scope_reset_child();
				logger_global_reset_child();
				logger_context_reset_child();
				logger_format_reset_child();
			}
			logger_runtime_publish(runtime);
			atomic_store_explicit(&owner_pid, (int)getpid(),
					      memory_order_release);
			atomic_store_explicit(&forked_child, 0,
					      memory_order_release);
		}
		/* 只有注册成功才发布 READY；显式调用可以重试正数错误票据。
		 * 注册成功后再也不进入此分支，不会重复安装处理函数。
		 */
		atomic_store_explicit(&process_ticket,
				      error ? error : PROCESS_READY,
				      memory_order_release);
		initializing = 0;
		rc = -error;
	}
	int restore = pthread_setcancelstate(old_cancel, NULL);
	return rc ? rc : -restore;
}

int logger_process_init(void)
{
	/* 新接口只返回 0/-errno；不让内部 libc 调用改写调用方的 errno。 */
	int saved = errno;
	int rc = initialize_process();

	errno = saved;
	return rc;
}

int logger_process_require(void)
{
	/* 普通调用只观察已发布的进程域，不注册、不重试，也不等待初始化者。
	 * READY 是 release/acquire 发布边界；成功后在当前进程中不再撤销。
	 * 子方必须先验证 atfork 快照并显式重建，不能等待已消失的线程。
	 */
	if (logger_process_is_child())
		return -ECHILD;
	if (initializing)
		return -EDEADLK;
	int ticket =
		atomic_load_explicit(&process_ticket, memory_order_acquire);

	if (ticket == PROCESS_READY)
		return 0;
	if (ticket == PROCESS_STARTING)
		return -EAGAIN;
	return ticket > 0 ? -ticket : -ENODEV;
}

int logger_process_object_acquire(void)
{
	unsigned n = atomic_load_explicit(&live_objects, memory_order_relaxed);
	do {
		if (n == UINT_MAX)
			return -EOVERFLOW;
	} while (!atomic_compare_exchange_weak_explicit(
		&live_objects, &n, n + 1, memory_order_acq_rel,
		memory_order_relaxed));
	return 0;
}

int logger_process_object_release(void)
{
	unsigned n = atomic_load_explicit(&live_objects, memory_order_acquire);
	for (;;) {
		if (!n)
			return -EUCLEAN;
		if (atomic_compare_exchange_weak_explicit(
			    &live_objects, &n, n - 1, memory_order_acq_rel,
			    memory_order_acquire))
			return 0;
	}
}

unsigned logger_process_object_count(void)
{
	/* 纯观测不能等待注册者；构造尚未发布时没有可借用的进程域。 */
	if (logger_process_is_child() || !logger_process_ready())
		return 0;
	return atomic_load_explicit(&live_objects, memory_order_acquire);
}

int logger_process_ready(void)
{
	return atomic_load_explicit(&process_ticket, memory_order_acquire) ==
	       PROCESS_READY;
}
