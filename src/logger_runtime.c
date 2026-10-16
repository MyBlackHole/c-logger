#define _GNU_SOURCE
#include "logger_runtime.h"
#include "logger_fork_fd.h"
#include "console.h"
#include "logger_process.h"
#include <errno.h>
#include <sys/mman.h>
#if defined(__SANITIZE_THREAD__)
#define LOGGER_RUNTIME_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LOGGER_RUNTIME_TSAN 1
#endif
#endif
#ifdef LOGGER_RUNTIME_TSAN
#include <sanitizer/tsan_interface.h>
/* 仅检测器构建保存已被 MADV_DONTFORK 移除的两把协调锁地址。
 * 内核移除映射不会经过 TSan 的 munmap 拦截器，因此当前线程的持锁表
 * 仍含 prepare 取得的租约。不能真实 munmap/解锁旧地址：它可能已被复用。
 */
static _Thread_local uintptr_t discarded_domain;

static void forget_discarded_leases(void)
{
	uintptr_t domain = discarded_domain;
	if (!domain)
		return;
	discarded_domain = 0;
	/* 按实际取得的逆序撤销旧进程租约；不解引用、重置任何真实锁，
	 * 不关闭报告或清空全部锁图。仅在有效 fork 子方的新域创建前执行一次。
	 * 新域的 pthread 初始化及全部锁操作仍由常规拦截器完整检查。
	 */
	void *resource =
		(void *)(domain + offsetof(struct logger_runtime, resource_mu));
	void *format =
		(void *)(domain + offsetof(struct logger_runtime, format_mu));
	__tsan_mutex_pre_unlock(resource, 0);
	__tsan_mutex_post_unlock(resource, 0);
	__tsan_mutex_pre_unlock(format, 0);
	__tsan_mutex_post_unlock(format, 0);
}
#endif

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
	       "fork root requires a lock-free pointer");
static _Atomic(struct logger_runtime *) current;
/* 初始化回滚的同步对象无法销毁时，保留候选而不是丢失所有者。 */
static struct logger_runtime *retained_bootstrap;

struct logger_allocation {
	struct logger_allocation *prev;
	struct logger_allocation *next;
	size_t bytes;
	max_align_t alignment;
};

struct logger_runtime *logger_runtime_current(void)
{
	return atomic_load_explicit(&current, memory_order_acquire);
}

void logger_runtime_publish(struct logger_runtime *runtime)
{
	atomic_store_explicit(&current, runtime, memory_order_release);
}

void logger_runtime_invalidate_child(void)
{
	/* 不解引用旧地址；对应映射已由内核在 fork 时排除。 */
#ifdef LOGGER_RUNTIME_TSAN
	if (logger_fork_fd_child_valid())
		discarded_domain = (uintptr_t)logger_runtime_current();
#endif
	atomic_store_explicit(&current, NULL, memory_order_relaxed);
}

int logger_runtime_discard(struct logger_runtime *runtime)
{
	int rc = 0;
	if (!runtime)
		return 0;
	if (runtime->initialized >= 5)
		rc = pthread_mutex_destroy(&runtime->console_mu);
	if (!rc && runtime->initialized >= 4)
		rc = pthread_rwlock_destroy(&runtime->global_lifetime_lock);
	if (!rc && runtime->initialized >= 3)
		rc = pthread_mutex_destroy(&runtime->global_control_mu);
	if (!rc && runtime->initialized >= 2)
		rc = pthread_mutex_destroy(&runtime->format_mu);
	if (!rc && runtime->initialized >= 1)
		rc = pthread_mutex_destroy(&runtime->resource_mu);
	if (!rc && munmap(runtime, sizeof(*runtime)))
		rc = errno;
	if (rc)
		retained_bootstrap = runtime;
	return -rc;
}

int logger_runtime_create(struct logger_runtime **out)
{
#ifdef LOGGER_RUNTIME_TSAN
	forget_discarded_leases();
#endif
	*out = NULL;
	if (retained_bootstrap)
		return -EUCLEAN;
	struct logger_runtime *runtime;
	runtime = mmap(NULL, sizeof(*runtime), PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (runtime == MAP_FAILED)
		return -errno;
	int rc = 0;
	if (madvise(runtime, sizeof(*runtime), MADV_DONTFORK))
		rc = errno;
	if (!rc) {
		rc = pthread_mutex_init(&runtime->resource_mu, NULL);
		if (!rc)
			runtime->initialized = 1;
	}
	if (!rc) {
		rc = pthread_mutex_init(&runtime->format_mu, NULL);
		if (!rc)
			runtime->initialized = 2;
	}
	if (!rc) {
		rc = pthread_mutex_init(&runtime->global_control_mu, NULL);
		if (!rc)
			runtime->initialized = 3;
	}
	if (!rc) {
		rc = pthread_rwlock_init(&runtime->global_lifetime_lock, NULL);
		if (!rc)
			runtime->initialized = 4;
	}
	if (!rc) {
		rc = pthread_mutex_init(&runtime->console_mu, NULL);
		if (!rc)
			runtime->initialized = 5;
	}
	if (rc) {
		(void)logger_runtime_discard(runtime);
		return -rc;
	}
	atomic_init(&runtime->resource_error, 0);
	atomic_init(&runtime->live_objects, 0);
	atomic_init(&runtime->global_ticket, 0);
	atomic_init(&runtime->global_lock_error, 0);
	atomic_init(&runtime->console_config,
		    ((unsigned)CONSOLE_NORMAL << 2) | CONSOLE_COLOR_AUTO);
	atomic_init(&runtime->console_lock_error, 0);
	*out = runtime;
	return 0;
}

static void link_allocation(struct logger_runtime *runtime,
			    struct logger_allocation *allocation)
{
	allocation->prev = NULL;
	allocation->next = runtime->allocations;
	if (allocation->next)
		allocation->next->prev = allocation;
	runtime->allocations = allocation;
	++runtime->allocation_count;
}

void *logger_runtime_calloc(size_t count, size_t size)
{
	if (!count || !size ||
	    size > (SIZE_MAX - sizeof(struct logger_allocation)) / count) {
		errno = EOVERFLOW;
		return NULL;
	}
	int error = logger_fork_resource_enter();
	if (error) {
		errno = error;
		return NULL;
	}
	struct logger_runtime *runtime = logger_runtime_current();
	size_t bytes = sizeof(struct logger_allocation) + count * size;
	struct logger_allocation *allocation;
	allocation = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (allocation == MAP_FAILED) {
		error = errno;
		allocation = NULL;
	} else {
		allocation->bytes = bytes;
		if (madvise(allocation, bytes, MADV_DONTFORK)) {
			error = errno;
			/* 失败仍由域持有；不把未配置的候选交给调用方。 */
			if (munmap(allocation, bytes)) {
				link_allocation(runtime, allocation);
				logger_fork_resource_poison(errno);
			}
			allocation = NULL;
		} else {
			link_allocation(runtime, allocation);
		}
	}
	error = logger_fork_resource_leave(error);
	if (error) {
		errno = error;
		return NULL;
	}
	return allocation + 1;
}

void *logger_runtime_malloc(size_t size)
{
	return logger_runtime_calloc(1, size);
}

int logger_runtime_free(void *ptr)
{
	if (!ptr)
		return 0;
	int error = logger_fork_resource_enter();
	if (error)
		return -error;
	struct logger_runtime *runtime = logger_runtime_current();
	struct logger_allocation *allocation =
		(struct logger_allocation *)ptr - 1;
	struct logger_allocation *prev = allocation->prev;
	struct logger_allocation *next = allocation->next;
	if (munmap(allocation, allocation->bytes)) {
		error = errno;
		logger_fork_resource_poison(error);
	} else {
		if (prev)
			prev->next = next;
		else
			runtime->allocations = next;
		if (next)
			next->prev = prev;
		--runtime->allocation_count;
	}
	/* 消费结果与协调锁错误分离：munmap 已成功时不能返回“未释放”，
	 * 否则上层会把已失效地址存成退役所有者。解锁失败仍毒化进程域，
	 * 最终实例销毁根据域诊断报告它，同时给出真实释放回执。
	 */
	(void)logger_fork_resource_leave(error);
	return -error;
}

/* 宿主必须先停止全部调用者并销毁实例，再卸载库；这里不接管业务线程。
 * 只归还空域，错误保留域绝不因卸载而尝试猜测其静默状态。
 */
static void __attribute__((destructor)) logger_runtime_unload(void)
{
	if (logger_process_is_child() || !logger_process_ready())
		return;
	struct logger_runtime *runtime = logger_runtime_current();
	if (!runtime ||
	    atomic_load_explicit(&runtime->live_objects,
				 memory_order_acquire) ||
	    atomic_load_explicit(&runtime->resource_error,
				 memory_order_acquire) ||
	    runtime->allocations || runtime->owned_count)
		return;
	logger_runtime_publish(NULL);
	(void)logger_runtime_discard(runtime);
}
