#ifndef LOGGER_PROCESS_H
#define LOGGER_PROCESS_H

/* Logger、Console 共享的私有进程级所有权守卫。
 * ensure 只在父进程侧完成注册，返回 0 / -errno。
 * 进入 pthread_once、锁、内存分配或 I/O 前必须先检查子进程状态。
 * 进程标识不在继承运行时的子进程中重新绑定；子进程必须 exec。
 * 这是防御性拒绝机制，不是通用的异步信号安全日志 API。
 */
int logger_process_ensure(void);
int logger_process_is_child(void);
void logger_process_invalidate_child(void);

/* 统计所有已创建或正在构造的 logger。
 * 这里只是进程级存活对象计数，不是对象引用计数，也不是生命周期获取与释放。
 * 计数归零不会触发析构；只有创建和销毁路径承担该成本，
 * 普通日志提交不受影响。
 */
int logger_process_object_acquire(void);
int logger_process_object_release(void);
unsigned logger_process_object_count(void);
#endif
