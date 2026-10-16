# 显式进程初始化与普通准入

## 宿主责任与边界

宿主在首次 Logger、Global、Console 或上下文操作前显式调用
`logger_process_init()`。成功后可以创建任意多个实例；进程初始化本身不创建
默认实例、日志文件、套接字或工作线程，也不替宿主安排业务线程。

```c
int rc = logger_process_init();
if (rc)
        return rc;
logger_t *owner = NULL;
rc = logger_create(&config, &owner);
if (rc)
        return rc;
/* 初始化业务 SDK，借用 owner；停止并等待借用者后销毁。 */
return logger_destroy(&owner);
```

`LOGGER_DEFAULT_CONFIG`、`CONSOLE_DEFAULT_CONFIG`、源位置值以及
`logger_redact`/`logger_mask_secret` 是不依赖运行域的辅助能力，无需初始化。
既有针对 NULL 输出参数的无操作规则不被转换为可观察的状态接口。

## 状态与线性化

| 进程状态 | 显式初始化 | 普通有状态调用的错误原因 |
|---|---|---|
| 尚未初始化 | 尝试构造和注册 | ENODEV；不建域 |
| 其他线程初始化中 | -EAGAIN | EAGAIN；不等待 |
| 初始化线程内重入 | -EDEADLK | EDEADLK；不递归 |
| 初始化失败、未发布 | 显式重试 | 上次保存的初始化错误；不重试 |
| 已就绪 | -EALREADY | 可继续各自的参数/实例/默认状态检查 |
| 有效 fork 后的子方、尚未重建 | 验证快照后重建 | ECHILD |
| 缺失有效快照的派生 | -ECHILD | ECHILD |

初始化票据的 READY 通过 release 发布；普通准入通过 acquire 读取，随后才能
读取运行域。CAS 争用的显式初始化不替竞争者等待。普通入口不调用初始化函数、
不循环让出 CPU，也不因失败或 PID 变化自动重试。成功发布后，当前进程的域不会
因实例数归零而撤销；卸载仍要求宿主证明所有 API/worker/fork 已静默。

普通操作的状态观测可能先于另一个线程完成发布，此时返回 EAGAIN 正是允许的
并发结果；宿主负责选择重试、报告或停止，库不提供隐藏退避循环。

## 错误与取消

`logger_process_init`、`logger_create`、`logger_destroy` 使用 0/-errno，
所有正常返回保留调用方 errno。其他公开接口仍遵守其当前约定，例如 Global 和
Console 的状态接口使用 -1/errno；void 入口通过既有 errno 约定报告拒绝。
本增量不冒称全部 API 的返回风格已统一。

进程准入先于读取配置、拥有者槽和实例字段，因而未初始化时 `create(NULL,NULL)`
与 `destroy(NULL)` 先报告 ENODEV。已就绪时仍执行原有 EINVAL/EBUSY/空槽合同。
拒绝不发布或消费拥有者，不格式化消息，不获取运行域锁，不改变线程取消策略。
指标入口按原规则清零有效输出缓冲，但不访问未发布或继承的实例。

初始化在取得票据前禁用取消，构造/注册完成并发布结果后恢复策略。取消恢复失败
可能报告错误但域已经就绪；后续显式调用的 -EALREADY 与普通准入结果反映已发布
状态。不能仅凭上次返回错误就断言进程完全未初始化。这里没有新增进程撤销 API。

## fork 与静态库副本

父方实例在子方失效；子方在 fork 返回后的普通线程上下文显式重建，不能在
atfork 回调内初始化，不复活或普通销毁旧实例。注册窗口、原始系统调用与无效资源
快照仍拒绝。完整 Linux/glibc 与宿主外部状态边界见 [运行域合同](FORK_RUNTIME.md)。

一份共享 DSO 的实例使用同一运行域。多个各自嵌入静态库的 SDK 有独立的库状态，
必须由宿主分别显式引导，不能在副本间转交私有实例指针。安装示例通过宿主调用
`plugin_process_init` 初始化该插件副本，再调用 `plugin_run`；这两个是示例 SDK
接口，不是新增 Logger 导出，也不是装载器构造函数或首次业务调用的隐式引导。

## 验证与未完成范围

`process-init` 分组验证公开冷启动拒绝、成功后可用、重复、注册失败/重试、
同线程重入、并发 EAGAIN、初始化取消、注册窗口和早注册 handler。生产静态与
白盒静态的冷拒绝探针在真实 mmap、atfork、mutex/rwlock、取消状态和 sched_yield
入口禁止副作用；并发测试要求注册者仍暂停时普通调用已经返回。

安装 C/C++11 可执行合同先验证未初始化创建/销毁的 ENODEV，再通过函数指针
显式初始化并实际创建/销毁。普通样例、基准、fuzz 初始化回调和每轮 dlopen 的
消费方均显式迁移；没有测试构造器或生产惰性兼容层。

默认实例借用层、其他返回形式统一、fork 检测器覆盖缺口和性能预算继续独立推进。
本合同和测试注册不代表任意提交已验证，实际结果必须绑定源码树和工具链。
