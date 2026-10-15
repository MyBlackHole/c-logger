# fork 子进程运行域重建：下一实施边界

## 当前状态

此文档是第三阶段的安全边界审查和实现拆分，不代表完整恢复已实现。第二阶段仅隔离继承的 Logger fd，子进程的普通 Logger API 仍返回 `-ECHILD`。

## 必须先满足的所有权约束

1. `logger_process_init()` 不能简单清空 `forked_child` 和 `owner_pid`。它还需要证明 Global/Console 锁、TLS 上下文、实例队列、worker、文件后端和进程域均与父进程断开。
2. 旧 `logger_t *` 只在父进程有效。子进程禁止对它执行正常日志、flush、destroy；子进程创建自己的实例。
3. 第二阶段的 `fd_mu` 在 child handler 中仍是继承的锁状态；即使 fd 位图清零，也不能复用旧锁或旧登记映射。应建立新进程域，令新的资源协调器由新域独占。
4. 所有运行时注册点必须覆盖构造中、销毁中和轮转中候选。不得只扫描运行实例链表。
5. 父进程原有日志输出和完成水位不变；子进程的新队列从空开始，不复制旧 worker 的消费位置。

## 建议的最小重构序列

- R1：将进程级 Global、Console、fd 协调器集中到独立 `logger_runtime`，只保留极小的 fork 检测引导状态。用明确的所有权标记保护发布。
- R2：验证资源快照的关闭错误、fd 复用、重复 fork 及候选路径；引导状态记录 `child_error`，失败不能放行。
- R3：为新运行域建立全新同步对象和资源表，完整构造后原子发布。不要通过 memset 修复父方 mutex。
- R4：子方显式 `logger_process_init()` 切换新域；之后 `logger_create()` 才允许创建新实例。旧实例不得复活。
- R5：静态/共享生产库分别验证单线程与多线程父方 fork、父方继续、子方真实日志、轮转冲突、失败回滚、重复派生、最低 glibc 和 Sanitizer。

## 特别风险：多线程 fork

即使 Logger 自身的 fd 已隔离，子进程中调用 malloc、pthread_create、localtime_r 和格式化函数的完整路径仍需针对 Linux/glibc 实际支持范围审查。不能把已完成 fd 隔离等同于完整可重初始化。

## 本阶段新增机械防退化门禁

`python3 tests/scripts/check_fork_reinit_boundary.py` 仅核对源码的五项局部不变量，防止在运行域隔离完成前意外放行子进程；它不是 AST 分析、运行时证明或多线程 fork 安全证明。真正恢复完成后，应以真实父子输出测试替换旧拒绝预期并调整门禁。
