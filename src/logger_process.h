#ifndef LOGGER_PROCESS_H
#define LOGGER_PROCESS_H

/* Logger、Console 共享的进程准入检查。
 * 当前既有入口仍可按需初始化，但只经过 logger_process_init 的单一状态机。
 * 普通调用不重试注册失败；显式 process_init 才能发起重试。
 * 普通调用在子方显式重初始化前拒绝；只有可信 atfork 快照允许显式
 * 初始化新的进程域。过程不复活继承实例，不宣称宿主全部 libc 状态已修复。
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
int logger_process_ready(void);
#endif
