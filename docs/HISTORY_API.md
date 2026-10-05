> 缺陷收敛开发版，仍非生产版 v1。当前 fork/生产构建规范见
> `docs/CONTROLLED_FORK_REINIT.md` 与 `docs/ROUND5_FORK_ISOLATION.md`。受控 fork 可重新初始化；原始 fork 仍拒绝继承运行时。
> 队列/刷新、Audit 状态、密码与解析分别见第 1–4 轮文档；剩余问题见 `KNOWN_ISSUES.md`。

# 公开 API 契约（v1 候选）

## 错误模型

公开构造函数成功时返回指针，失败时返回 `NULL` 并设置 `errno`。
公开的 `int` 类型控制/Audit/Console 操作统一使用成功返回 `0`、失败返回 `-1` 并设置
`errno` 的约定；唯一例外是作为高级兼容 API 保留的 `logger_log_sync_status()`，
它返回 `0` 或负 errno。

## 所有权

`logger_create()` 将 Logger 实例的所有权转移给调用方，`logger_destroy()` 负责释放。
在销毁前，所有者必须停止并等待退出所有仍使用显式 `logger_t *` 的线程。销毁完成后，
原始 C 指针立即失效。

全局 `LOG_*` 门面由库内部管理生命周期同步，因此允许与 `logger_shutdown()` 发生并发竞争，
其具体行为受全局生命周期契约约束。

## 线程模型

对于处于活动状态的 Logger，日志写入、级别修改、指标读取和刷新设计为可并发调用。
文件输出在内部串行化；异步记录进入有界 MPSC 队列。

## Fork

这里有两种**有意不同**的操作模型。

### 受控 fork、无 exec：`pid_t logger_fork_reinit(void)`

这个显式启用辅助接口支持“初始化后执行受控 fork，并在父、子进程中分别重新初始化”。
可选的进程全局 Logger 会在 fork **之前**完成排空、同步、销毁，并等待工作线程退出。
fork 返回后的父、子分支都不存在默认 Logger，必须分别显式调用 `logger_init()`。
任何继承的队列、文件描述符、工作线程或 Logger 对象都不会复用。

调用前必须满足以下前置条件：先等待应用线程退出、关闭 Audit，并销毁所有显式拥有的 Logger。
该辅助接口必须由最后剩余的应用线程调用；进入时只允许可选的全局 Logger 工作线程仍然存在。
其他库也必须已经静默，并且能够支持单线程 fork/继续执行。执行期间，信号处理器和 atfork
处理器**不得**使用本库，也不得创建线程。它不是面向任意活动多线程程序的通用 fork 包装器。

拆除前会检查资源计数以及 `/proc/self/task`。如果存在额外实例或额外线程，会以 EBUSY
返回，并且不会触碰默认 Logger。完成拆除后，在真正执行 fork 前还会再次检查任务数量，
要求 Linux 线程数严格为 1。这是失败关闭检查，**不能**替代应用自身的同步，
也不能证明第三方库不存在锁状态；同时要求 `/proc` 可用。

返回值遵循 fork 风格：父进程返回 `>0`，子进程返回 `0`，失败返回 `-1` 并设置 errno。
拆除/同步失败、拆除后的验证失败，或者 fork 本身失败，都可能使全局 Logger 保持停止状态。
没有自动回滚或配置恢复机制。服务如何恢复由所有者决定；不能假定函数失败返回后旧 Logger
仍然处于活动状态。

已注册的子进程防护在 atfork 处理器阶段仍会执行失效处理。只有该辅助接口私有的
干净 fork 令牌才允许 fork 返回后重新绑定进程身份。继承的 `pthread_once` 此时已经执行完成，
相关锁也已处于解锁状态；不会覆盖或重新初始化任何 pthread 对象。令牌在每个进程中只能消费一次。
子进程的请求上下文会清空；父进程上下文和 Console 配置继续保留。
执行 `logger_fork_reinit()` 后不要再调用旧的 after-fork 失效辅助接口，因为新辅助接口已经完整执行协议。
随后可以重新创建显式 Logger，Audit 也会创建全新会话。父、子进程的 Audit 目标应当不同；
同一条链的单写入器锁仍然生效，不会因为进程身份重新绑定而绕过。

### 运行时已经使用后的原始 `fork()`

第 5 轮的防御行为继续保留：子进程中的构造函数/状态操作会在接触继承锁、once、分配或 I/O
之前返回 ECHILD。void 运行时 API 会直接空操作并设置 ECHILD。
`logger_log_sync_status()` 继续保留“返回负 errno”的历史风格。对于原始 fork，普通关闭
并不会解除粘滞的进程身份状态。调用 `logger_after_fork_child()` 或 `audit_after_fork_child()`
也不会允许重新初始化。`logger_prepare_fork()` / `logger_after_fork_parent()` 继续只是历史标记，
它们不会排空工作线程，也不实现上述受控辅助接口。

如果确实保持严格单线程，可以在任何 Logger 运行时使用之前 fork；fork+exec 也继续受支持。
不支持 vfork、原始 clone、信号处理器中写日志，也不支持任意多线程程序在 fork 后重建运行时。
`LOG_*` 宏参数会在库执行拒绝检查之前由 C 语言求值。详见 `docs/CONTROLLED_FORK_REINIT.md`。

## Audit 持久性与故障状态

严格文件输出必须成功后，已提交 sequence/hash 才能推进。检查点失败会使追加操作进入
`CHECKPOINT_FAILED` 暂停状态，同时把已经确认的链头保留在内存中；`audit_flush()` 只修复这个
派生检查点，绝不会重写已经写出的事件。log write/fsync 失败会保守地进入 `IO_FAILED`：
即使后续系统调用恢复正常，该运行时也不再允许 append/STOP。此时应停止、检查并协调已有数据，
然后重新初始化。

`audit_get_status()` 只是诊断快照，不是并发事务回执。`audit_shutdown_status()` 会释放运行时所有权，
并报告 STOP/检查点或已经保存的 I/O 故障；void 版关闭 API 只是兼容包装器。
即使某条记录已经提交，后续函数仍可能返回失败，因此不能因为失败返回就自动重放该记录。
密码和恢复路径仍存在已知发布阻断项，应结合对应轮次文档判断。

## Console

`console_print()` 是稳定的 stdout 数据/结果通道；面向人的诊断信息使用 stderr。
运行时 Logger 元数据不得混入 stdout 结果。

## 配置 ABI

`logger_config_t`、`audit_config_t` 和 `console_config_t` 都包含 `struct_size` 与 `version`。
默认宏会填充两者。未知版本会被拒绝；带版本但结构尺寸不足的对象也会被拒绝。
为了兼容手工清零初始化的源码调用方，目前仍临时保留 `0/0` 兼容路径。

## Audit 单写者约束

`audit_init()` 会对 `<log_dir>/<name>.audit.lock` 获取非阻塞 POSIX
`fcntl(F_SETLK)` 排他锁。竞争进程会收到 `EBUSY`。锁文件描述符由对应运行时持续持有，
直到输出资源处置完成。初始化和关闭共享同一套控制协议，因此旧运行时的清理不能释放新运行时的锁。
原始 fork 子进程必须先 exec 才能创建 Audit。通过 `logger_fork_reinit()` 成功完成受控 fork 的子进程
可以在 fork 后创建全新 Audit，因为进入辅助接口前 Audit 已经关闭。fork+exec 路径仍要求 CLOEXEC。

咨询锁只能协调遵守协议的本地进程，不能阻止任意直接写入者。应用不得在其他位置自行打开/关闭
或替换当前活动 锁 inode。

锁文件只是协调元数据，不是完整性证据；文件存在本身并不代表锁当前确实被持有。

## Syslog 后端

`LOGGER_OUT_SYSLOG` 为每个 `logger_t` 使用独立、已 连接 的 Unix 数据报 套接字，
而不是 libc `openlog()/syslog()/closelog()`。因此不同实例拥有彼此独立的身份与生命周期状态。
Linux 默认目标为 `/dev/log`。如果无法连接已配置的 Syslog 输出端，Logger 构造会失败，
而不是静默假装该后端存在。

## Audit 摘要提供方

Audit 完整性使用提供方模型。SHA-256 与 SM3 当前向 chain/检查点/恢复代码提供相同的
32 字节摘要契约。检查点 v2 会记录算法标识，避免一条链被使用不同摘要算法重新打开。
`audit_verify_file_with()` 用于显式验证非默认算法。

## 生产密码后端

Audit 摘要算法选择与密码实现后端彼此独立。`LOGGER_CRYPTO_BACKEND=builtin` 使用自包含的
SHA-256/SM3 实现；`LOGGER_CRYPTO_BACKEND=openssl` 通过 OpenSSL EVP 计算 SHA-256 和 SM3，
并链接 `OpenSSL::Crypto`。`audit_crypto_backend()` 用于暴露当前选择的后端，方便诊断与合规清点。

Audit chain/检查点格式记录的是摘要算法，而不是具体库后端，因此 SHA-256 或 SM3 链可以在
兼容的后端实现之间迁移。SHA-256("abc") 和 SM3("abc") 的已知答案测试会同时约束两种后端。

## 敏感数据

本库使用显式脱敏，而不是启发式敏感信息检测。`logger_redact()` 返回统一的脱敏标记；
`logger_mask_secret()` 创建部分遮蔽表示，同时保证不会完整暴露输入值。Audit 字段只允许承载元数据；
构造事件之前，调用方必须先移除秘密材料。详见 `SECURITY.md`。

## 重新初始化契约

进程全局 Logger 和 Audit 在每个活动生命周期中都只能初始化一次。第二次初始化或并发初始化会返回
`-1` 并设置 `errno=EALREADY`。同一进程中关闭后允许再次初始化。
如果多个初始化并发而期间没有关闭，只允许一个候选成功发布。Audit 会串行化完整生命周期；
普通全局 Logger 仍会在发布锁之前构造候选实例，因此初始化/关闭同时发生仍属于已知限制，
见 `KNOWN_ISSUES`。Console 初始化继续允许重新配置，其原子设置可以重复更新。

## 显式多实例日志

`logger_init()` 只拥有可选的进程全局默认 Logger，并且在每个活动生命周期中仍然只能初始化一次。
独立 Logger 使用 `logger_create()` 创建，使用 `logger_destroy()` 销毁。

对于显式实例，公开便利宏会保留源码位置信息：

```c
LOGGER_INFO(network_logger, "network", "connected fd=%d", fd);
LOGGER_ERROR(storage_logger, "storage", "write failed errno=%d", err);
```

`LOGGER_LOG(instance, level, module, ...)` 是通用形式。这些宏不会引入新的全局状态，
只是转发到 `logger_log()`。所有者必须在销毁显式实例前停止并等待所有使用者退出。

## Audit 生命周期准入

START 以及对应检查点必须成功后，运行时才允许发布。STOPPING 会先关闭新操作准入，
再等待当前操作结束；成功写出的 STOP 必须是最后一条记录。所有 seq/txn、编码和提交状态都进行串行化。
初始化失败不会写 STOP。跨越关闭/reinit 代次的延迟调用返回 ESHUTDOWN。
并发初始化和关闭会进行显式串行化，不能只凭某个指针检查就假定安全。
状态字段、取消语义和兼容性说明见 `docs/ROUND2_AUDIT_STATE.md`。

## 第 3 轮：摘要错误与 CRYPTO_FAILED

私有摘要回调现在返回 `0 / -errno`，失败时保持输出缓冲区不变。所有写入、恢复和验证器调用方
都会检查该返回值。EVP 失败绝不会用全零摘要替代，不会回退到内置实现，不会切换已选择的算法，
也不会关闭完整性。公开 Audit API 仍采用 POSIX 风格的 `0 / -1 + errno`；`CRYPTO_FAILED`
用于区分密码引擎问题与结果不确定的后端 `IO_FAILED`。上下文分配失败映射到 ENOMEM，
EVP 方法缺失映射到 ENOTSUP，其他 EVP 阶段/长度错误映射到 EIO。

运行时密码故障一旦发生，就会一直保持到关闭/重新初始化。发生故障的操作不会推进记录、
`committed_seq`、链头或检查点；`audit_flush` 无法清除该状态，`shutdown_status`
会报告故障且不会伪造 STOP。新 enum 值追加在末尾，现有数值和状态布局保持不变。
初始化副作用、事务 ID 预留、验证输出和历史坏日志限制详见 `docs/ROUND3_CRYPTO.md`。

## 第 4 轮：严格 Audit 记录与恢复

验证器/恢复现在共用一套有界 KV 编解码器。字段顺序、带引号转义、整数范围、严格 64 个小写
十六进制摘要字符以及最终 LF 都会验证。检查点 v2 中的尾随字节/额外行会被拒绝。
格式非法记录返回 EBADMSG，输入过大返回 EOVERFLOW，底层 I/O/密码失败保留原始错误。
没有新增公开函数或结构布局。

恢复过程会验证所有保留分段，并根据已经验证的记录 offset/sequence/hash 定位检查点。
分段顺序由唯一摘要边决定，而不是文件名时间或文件大小。真正位于 EOF 的残片，如果满足修复条件，
会先保存到唯一的 `0600` `.tail.*` 文件并完成同步，然后才截断活动文件。
这并不能证明一定发生了崩溃，也不是跨文件原子回滚。对于已经包含完整 hash、仅缺 LF 的记录，
会直接拒绝，而不是静默删除。

历史 Logger 前缀仍处于摘要域之外。非规范旧控制字符/字段、保留策略后已经无法获得的根、
不可信历史以及 v1 检查点都不会自动转换。限制条件、`O(total retained bytes + N²)` 启动工作量、
证据处理，以及必须满足的单写入器/可信目录假设，见第 4 轮文档。

## 生产代码与测试代码隔离

`logger` / `liblogger.a` 始终是生产目标。无论是否启用 `BUILD_TESTING`，其中都不存在环境变量驱动的
故障/崩溃实现或对应符号。启用 `BUILD_TESTING=ON` 后，独立的 **logger_test_support** 归档库
为明确选择的测试提供历史故障/崩溃钩子和测试 Syslog 路径覆盖能力。
普通回归、示例和基准都链接生产目标。绝不能同时链接两种变体：CMake 会报告不兼容的
`LOGGER_VARIANT` 接口。不存在任何运行时选项或 CMake cache 开关可以把生产 Logger 变成故障注入构建。

只需要库本身时使用 `-DBUILD_TESTING=OFF`。`LOGGER_SYSLOG_PATH` 不再是生产环境变量覆盖项；
生产环境固定使用 `/dev/log`。将非测试 套接字 路径做成可配置项仍属于未来 API 设计方向，
不会通过隐式环境变量覆盖实现。
