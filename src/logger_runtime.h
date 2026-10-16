#ifndef LOGGER_RUNTIME_H
#define LOGGER_RUNTIME_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>

struct logger;
struct logger_allocation;

/* 整个对象采用独立 MADV_DONTFORK 映射。只有 READY 发布后才可借用；
 * 进程存续期间地址不变，销毁单个实例不会释放这个域。
 */
struct logger_runtime {
	pthread_mutex_t resource_mu;
	pthread_mutex_t format_mu;
	pthread_mutex_t global_control_mu;
	pthread_rwlock_t global_lifetime_lock;
	pthread_mutex_t console_mu;
	unsigned initialized;
	_Atomic int resource_error;
	uint64_t *owned_fds;
	size_t owned_bytes;
	size_t owned_count;
	struct logger_allocation *allocations;
	size_t allocation_count;
	_Atomic unsigned live_objects;

	struct logger *global_logger;
	struct logger *global_retired;
	_Atomic uint64_t global_ticket;
	_Atomic int global_lock_error;
	_Atomic unsigned console_config;
	_Atomic int console_lock_error;
};

/* 引导专用：创建、回滚和发布不经过可能尚未存在的资源锁。 */
int logger_runtime_create(struct logger_runtime **out);
int logger_runtime_discard(struct logger_runtime *runtime);
struct logger_runtime *logger_runtime_current(void);
void logger_runtime_publish(struct logger_runtime *runtime);
void logger_runtime_invalidate_child(void);

/* 私有映射分配器：取得、配置 DONTFORK 和登记由资源锁串行化。
 * 返回存储具备 max_align_t 对齐；不使用宿主堆页或替换宿主分配器。
 * 释放失败保留登记、毒化域；返回值描述映射是否已消费。
 * 实际释放后的解锁错误由域粘滞诊断报告，不返回未消费的假回执。
 */
void *logger_runtime_calloc(size_t count, size_t size);
void *logger_runtime_malloc(size_t size);
int logger_runtime_free(void *ptr);

void logger_context_reset_child(void);
void logger_format_reset_child(void);
void logger_scope_reset_child(void);
int logger_global_scope_busy(void);
void logger_global_reset_child(void);
#endif
