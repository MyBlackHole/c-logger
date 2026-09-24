#ifndef LOGGER_PROCESS_H
#define LOGGER_PROCESS_H
#ifndef LOGGER_ENABLE_LEGACY_FORK_HELPER
#define LOGGER_ENABLE_LEGACY_FORK_HELPER 0
#endif

/* Logger、Console、Audit 共享的私有进程级 ownership guard。
 * ensure 只在 parent 侧完成注册，返回 0 / -errno。
 * 进入 pthread_once、锁、内存分配或 I/O 前必须先检查 child 状态。
 * process identity 只允许在成功准备的 clean fork 后重新绑定。
 * 这是防御性拒绝机制，不是通用 async-signal-safe 日志 API。
 */
int logger_process_ensure(void);
int logger_process_is_child(void);
void logger_process_invalidate_child(void);

/* 统计所有已创建或正在构造的 logger，包括 Audit 私有 logger。
 * 这里只是 process census，不是对象 refcount，也不是 lifetime get/put。
 * 计数归零不会触发析构；只有 create/destroy 路径承担该成本，
 * 普通日志提交不受影响。 */
int logger_process_object_acquire(void);
void logger_process_object_release(void);
unsigned logger_process_object_count(void);
#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_process_thread_count(unsigned *out); /* Linux /proc，实现返回 0 / -errno */
int logger_process_arm_clean_fork(void);
int logger_process_finish_clean_fork(void);

#endif /* legacy helper */
#endif
