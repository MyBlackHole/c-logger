#include "logger_process.h"

#include <errno.h>
#include <limits.h>
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
#include <dirent.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

/* Linux 的 pid_t 可放入 int。handler state 与 process identity 必须 lock-free；
 * child rejection 路径禁止经过 libatomic 内部锁。 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "fork guard requires lock-free int");
static _Atomic int owner_pid;
static _Atomic int forked_child;
/* 进程级对象 census，只用于 fork/lifecycle gate。
 * 这不是 per-logger refcount：它不会延长任何 logger_t 的生命周期，
 * 计数归零也不会触发对象 release。 */
static _Atomic unsigned live_objects;
/* 只有 logger_fork_reinit 在确认全部对象已销毁后才会置为非 0。
 * fork 后复制出的 token 在父子进程中分别独立消费。 */
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
static _Atomic int clean_fork_owner;
#endif
static pthread_once_t register_once = PTHREAD_ONCE_INIT;
static int registration_error; /* published by pthread_once */

void logger_process_invalidate_child(void)
{
	/* 这里绝不能覆盖/销毁 pthread 对象，也不能释放继承来的 fd。
	 * inherited runtime 保持原样，直到 exec 或 _exit。 */
	atomic_store_explicit(&forked_child, 1, memory_order_relaxed);
}

int logger_process_is_child(void)
{
	if (atomic_load_explicit(&forked_child, memory_order_relaxed))
		return 1;
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	/* 同时覆盖应用 child handler 早于本库注册、以及注册窗口内 fork 的情况。
	 * 这里不承诺支持 vfork/clone。 */
	return owner != 0 && owner != (int)getpid();
}

static void register_guard(void)
{
	registration_error =
		pthread_atfork(NULL, NULL, logger_process_invalidate_child);
}

int logger_process_ensure(void)
{
	if (atomic_load_explicit(&forked_child, memory_order_relaxed))
		return -ECHILD;
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	int current = (int)getpid();
	if (owner != 0 && owner != current)
		return -ECHILD;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(&owner_pid, &expected,
						      current,
						      memory_order_release,
						      memory_order_relaxed);
	if (expected != 0 && expected != current)
		return -ECHILD;
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
	if (atomic_load_explicit(&clean_fork_owner, memory_order_acquire))
		return -EBUSY; /* 仅 legacy wrapper 使用 */
#endif
	/* 先 publish process identity，再进入 pthread_once 注册。
	 * 如果 fork 发生在其他线程安装 handler 期间，child 不能进入继承来的 once lock。 */
	int rc = pthread_once(&register_once, register_guard);
	return rc ? -rc : -registration_error;
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

void logger_process_object_release(void)
{
	(void)atomic_fetch_sub_explicit(&live_objects, 1, memory_order_acq_rel);
}

unsigned logger_process_object_count(void)
{
	return atomic_load_explicit(&live_objects, memory_order_acquire);
}

#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_process_thread_count(unsigned *out)
{
	if (!out)
		return -EINVAL;
	DIR *dir = opendir("/proc/self/task");
	if (!dir)
		return -errno;
	unsigned count = 0;
	int error = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			error = errno;
			break;
		}
		const char *p = entry->d_name;
		if (!*p)
			continue;
		while (*p >= '0' && *p <= '9')
			++p;
		if (*p)
			continue;
		if (count == UINT_MAX) {
			error = EOVERFLOW;
			break;
		}
		++count;
	}
	if (closedir(dir) != 0 && !error)
		error = errno;
	if (error)
		return -error;
	if (!count)
		return -EIO;
	*out = count;
	return 0;
}

int logger_process_arm_clean_fork(void)
{
	if (logger_process_is_child())
		return -ECHILD;
	if (logger_process_object_count())
		return -EBUSY;
	int expected = 0;
	if (!atomic_compare_exchange_strong_explicit(
		    &clean_fork_owner, &expected, (int)getpid(),
		    memory_order_release, memory_order_relaxed))
		return -EALREADY;
	return 0;
}

int logger_process_finish_clean_fork(void)
{
	int prepared =
		atomic_load_explicit(&clean_fork_owner, memory_order_acquire);
	int owner = atomic_load_explicit(&owner_pid, memory_order_acquire);
	if (!prepared || prepared != owner || logger_process_object_count())
		return -ECHILD;
	/* wrapper 已建立“单线程 + 0 个对象”的边界。pthread_once 已完成，
	 * 库内 mutex 均未被持有。不要重新赋 pthread initializer，
	 * 不要销毁继承来的锁，也不要复用旧 fd。
	 * 该函数只能由受控 wrapper 调用，不能从应用 handler 直接调用。 */
	atomic_store_explicit(&owner_pid, (int)getpid(), memory_order_release);
	atomic_store_explicit(&forked_child, 0, memory_order_relaxed);
	atomic_store_explicit(&clean_fork_owner, 0, memory_order_release);
	return 0;
}

#endif
