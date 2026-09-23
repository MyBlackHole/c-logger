> 本轮更新：此文描述 **opt-in 兼容 helper**，不再是第三方集成默认入口。
> 需要 `-DLOGGER_ENABLE_LEGACY_FORK_HELPER=ON`；默认产物不编译该函数。
> 新集成参见 [HOST_OWNED.md](HOST_OWNED.md)。下文保留原兼容约束。

# 初始化后 fork 并重新初始化（受控模式）

## 能力

新增 `pid_t logger_fork_reinit(void)`。面向 CLI、daemon、prefork worker：程序
已经用过同步/异步默认 Logger，但需要不 exec 的子进程业务流程。默认 Logger
可以仍处于 RUNNING；wrapper 在父进程正常环境中排空、同步、join worker、关闭
后端并释放对象，然后才进行 fork。两条返回路径均主动调用 `logger_init()`。

这里不是从子进程“抢救”一份继承的并发运行时，而是 fork 时已经没有该运行时。
不重置 pthread mutex/rwlock，不泄漏旧 logger 对象，不保留旧 worker/queue/fd。

## 调用前置条件

1. 停止并 join 所有业务线程；不能只暂停持锁线程。确保没有第三方后台线程；
   其他依赖也必须允许其单线程状态下 fork/continue。
2. 检查 `audit_shutdown_status()`，按业务策略处理失败。不要跨 fork 保留未完成
   ATTEMPT/RESULT 事务。显式 `logger_t*` 由 owner 检查 flush 后 destroy 并置空。
3. 只剩一个调用线程以及可选默认 Logger worker。不得在本函数调用期间从信号
   或 atfork handler 使用本库、创建线程。它不是信号处理 API。
4. Linux `/proc/self/task` 可读。计数是额外防御，不是并发应用的同步方法，更不
   是证明所有外部 mutex 一定可用的安全判定。

## 用法

```c
logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
cfg.outputs = LOGGER_OUT_FILE;
cfg.file_path = "parent.log";
if (logger_init(&cfg) != 0) { /* 处理错误 */ }
LOG_INFO("before fork");

/* 到这里，业务线程已 join，Audit/显式实例已关闭。 */
pid_t pid = logger_fork_reinit();
if (pid < 0) {
    /* errno 说明失败原因；旧默认Logger可能已被清理，不能自动假设仍然可用。 */
}
/* 父/子都需要重新初始化，不再调用旧 after_fork_* helpers。 */
cfg.file_path = pid == 0 ? "child.log" : "parent.log";
if (logger_init(&cfg) != 0) { /* 处理错误 */ }
```

完整可编译示例是 `examples/fork_reinit.c`，包含失败退出和 waitpid。
新旧默认实例不是同一对象。显式实例的旧指针已在 fork 前销毁，不得再访问；
需要重新 `logger_create()` 并传给模块。父子不应独立轮换同一个普通日志路径。

## 确切顺序

```
process ensure / 禁止取消
    -> trywrlock global，检查活跃logger计数与task数量
    -> 关闭默认入口
    -> drain / join worker / sync / close / free
    -> 释放global锁
    -> 再检查task数量=1（只容许有限的已退出worker procfs清理延迟）
    -> 私有one-shot clean-fork token
    -> fork
    -> child handler照常拒绝继承调用
    -> fork返回后消费token，绑定本进程PID
    -> child清除request context
    -> 恢复取消状态，返回pid
```

实例计数包含初始化中/失败清理中的 logger，也包含 Audit 的内部 logger。
有未关闭的显式实例或Audit时预检查返回EBUSY，不偷偷替它们做销毁。
库注册handler失败则直接传播错误。token存在期间，父进程atfork handler中的新
runtime调用返回EBUSY；子进程仍返回ECHILD。正常调用不需要知道token。

## 错误与副作用

- 预检查EBUSY、/proc读取失败：默认Logger未改变。
- 默认输出/同步/关闭失败：停止并清理默认Logger，返回错误，不fork。
- teardown之后发现额外task或fork返回EAGAIN：默认Logger已停止，返回错误；
  调用者决定是否显式重新初始化。不会自动恢复配置，也不伪造一次成功fork。
- 阻塞文件/管道/syslog仍可使排空延迟；本轮没有增加通用I/O deadline。
- 父子取消请求不是对已经发生fork或已提交日志的回滚。
- 文件/队列/worker均不跨界继承；进程其他非本库fd由应用自行管理。
- Console配置保留，child request/session/trace context清空，parent context保留。
- Audit重建新instance ID。允许父子各有Audit，但同一chain仍只能一个writer；
  冲突返回EBUSY。这不是对过去故障状态或可疑历史的自动解除。

## 不能扩大成什么承诺

`logger_init(); fork(); logger_init();` 使用的是 raw fork，仍会被拒绝。
需要使用这个受控helper，或在真正单线程且尚未使用库运行时之前fork，或fork+exec。
不支持仍活跃的任意多线程应用 fork 后任意C库调用；不靠覆写继承mutex“修复”。

TSan自身可能在创建过线程后留有后台线程。此时单线程检查会正确拒绝；不能忽略
这些线程强行fork。当前结果、失败日志和不链接logger的独立探针见验证报告。

本轮是新增受控能力，原Raw-fork拒绝与生产fault隔离测试仍保留。
