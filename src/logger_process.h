#ifndef LOGGER_PROCESS_H
#define LOGGER_PROCESS_H

/* Logger、Console 共享的私有进程级 ownership guard。
 * ensure 只在 parent 侧完成注册，返回 0 / -errno。
 * 进入 pthread_once、锁、内存分配或 I/O 前必须先检查 child 状态。
 * process identity 不在继承运行时的 child 中重新绑定；child 必须 exec。
 * 这是防御性拒绝机制，不是通用 async-signal-safe 日志 API。
 */
int logger_process_ensure(void);
int logger_process_is_child(void);
void logger_process_invalidate_child(void);

/* 统计所有已创建或正在构造的 logger。
 * 这里只是 process census，不是对象 refcount，也不是 lifetime get/put。
 * 计数归零不会触发析构；只有 create/destroy 路径承担该成本，
 * 普通日志提交不受影响。 */
int logger_process_object_acquire(void);
int logger_process_object_release(void);
unsigned logger_process_object_count(void);
#endif
