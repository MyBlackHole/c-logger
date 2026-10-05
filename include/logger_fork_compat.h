#ifndef LOGGER_FORK_COMPAT_H
#define LOGGER_FORK_COMPAT_H
#include "logger_export.h"
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 仅供应用使用的辅助接口，不属于默认库。已有使用方必须配置
 * LOGGER_ENABLE_LEGACY_FORK_HELPER=ON（保留原有语义）。
 * 优先采用宿主负责进程创建、显式管理实例生命周期的模型。 */
/* 面向已经静默的 Linux 应用执行不带 exec 的受控 fork。
 * 进入前：停止/等待退出每一个可能使用本库的应用线程；销毁**全部**显式 logger_t 实例并关闭 Audit。
 * 只允许可选默认 Logger 及其工作线程仍然存在。本调用期间，不得在线程/信号/atfork 处理器中
 * 创建线程或使用本库。其他库也必须静默，并允许单线程 fork/继续执行。
 *
 * 本函数会在 fork **之前**排空/同步/销毁默认 Logger，检查 /proc/self/task 是否已经达到单线程边界，
 * 然后调用 fork()。父、子返回分支都可以调用 logger_init/create、console_init 和 audit_init。
 * 父子必须分别显式重新初始化；旧实例不会复用。子进程中绝不会重置运行时计数器/锁。
 * 子进程请求上下文会清空；父进程请求上下文和 Console 配置保留。
 *
 * 返回值：父进程中返回 >0 的子 PID，子进程返回 0，失败返回 -1 + errno。
 * EBUSY 表示仍有其他线程/实例存在；要求 /proc 可用。拆除之后的失败（包括 fork 失败）
 * 会让默认 Logger 保持关闭，**不会**自动恢复。原始 fork 仍保留继承状态防护。
 */
LOGGER_API pid_t logger_fork_reinit(void);

#ifdef __cplusplus
}
#endif
#endif
