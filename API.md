# 当前 API 与集成边界

本文描述 main 的普通 Logger/Console 接入；当前开发 ABI 为 SONAME 2 / `LOGGER_2.0`，
默认47个公开 C 符号，清单见 `abi/logger-2.0.symbols`。2.0 最终公开合同尚未冻结。
Audit 不在生产库或安装头文件中；已发布 v1 的62符号合同仅适用于 `release/1.x`。
`logger_fork_reinit()` 仍仅由 opt-in 兼容产物提供，不进入默认 ABI。

## 显式实例（第三方默认入口）

- `logger_create()` 返回宿主拥有的实例；失败 NULL + errno。
- `logger_log()`、`logger_log_source()`、`LOGGER_LOG/INFO/...` 使用指定实例。
- `logger_flush_instance_status()` 返回 0 / -1 + errno，等待既定完成水位并同步文件。
  不代表远端 syslog 已持久化，也不提供阻塞 I/O 超时。
- `logger_destroy_status()` 是独占 owner 的关闭/释放接口；调用前先停止并 join 所有使用者。
  排空、join、同步、关闭后端后释放内存；普通 I/O 失败仍释放实例，返回 -1 + errno，
  不能重试旧指针。NULL 在原进程中成功；ECHILD 拒绝继承状态，不碰旧资源。
  同步对象销毁/join等释放证明失败时，优先保留内部容器而不冒险释放；这不授权重试旧句柄。
  极少发生的 pthread 取消状态设置失败也会在处置前返回。
- `logger_destroy()`、`logger_flush_instance()` 保留为丢弃状态的兼容 wrapper。
- `logger_log_sync_status()` 保留历史 `0/-errno` 约定，不能和 POSIX 风格状态接口混淆。
- Live metrics 是并发近似快照，不是单笔业务提交证明。

生命周期中不得在仍有 caller/worker 使用句柄时 free、重建或卸载库。
不增加 init 引用计数，不隐式夺取借用句柄所有权。

## 诊断与可观测性

`logger_get_diagnostics()` 提供无需获取后端锁的统一运行状态快照，包括队列深度/容量、
完成积压、溢出区使用/耗尽、工作线程等待/唤醒、丢弃/回退、输出失败和
粘滞首个错误。它不执行刷新、不执行重连、不进行文件/网络 I/O，也不清理错误状态。

`logger_get_global_diagnostics()` 使用现有全局代次/生命周期固定机制；不会越过
STARTING/STOPPING 准入门。文件/Syslog 的完整一致输出端指标分别由
`logger_get_file_metrics()` / `logger_get_syslog_metrics()` 提供，因为 backend
计数器由 `emit_mu` 保护。文件快照不执行 write/fsync/rotation/reopen，只读取
累计事实；全局门面可用 `logger_get_global_file_metrics()` 通过当前代次固定机制
读取同一份 File 状态。

问题发现与采样语义见 [OBSERVABILITY.md](docs/OBSERVABILITY.md)。

## 普通日志字符串寿命

调用者的 fmt/source/context 输入必须在对应调用期间有效，不得并发修改。
用户消息仍通过 `vsnprintf` 复制。元数据现在按不可变的 `detail/include_*` 需求采集：
只有最终格式真正会使用的模块/上下文/源信息才进入异步队列；需要保留的
模块/源文件基础名/函数继续做有界快照，入队后不再保留业务 DSO 的源信息指针。
容量（不含 NUL）：模块 127、基础文件名 255、函数 127 字节。
超限在末尾以 `~` 标识缩短，按字节而不是 Unicode 代码点处理；这是有意的可见边界变化。
仅作为元数据标签，不能拿被缩短的字符串作业务标识或安全判据。

不新增每条日志的堆分配。私有 record 增加 512 字节；内部输出行缓冲变为 5120 字节，
容量 >=2 时保留 LF+NUL，0/1 容量也有界处理。正常长度的原有输出字节保持不变。
运行时变更 TZ 的缓存语义未在本轮设计。

## 全局与 Console

全局 `logger_init/LOG_*/logger_shutdown` 保留便利用法；业务 `.so` 不应偷偷调用。
全局生命周期遵循 docs/GLOBAL_LIFECYCLE.md；显式实例仍由宿主独占销毁。
Console 是应用终端展示工具，不是业务 SDK 隐式输出通道。

## Fork 与兼容

默认库保留 PID/atfork 误用防护，但不替宿主 fork 或检查全进程线程列表。
一般 raw fork 的 ECHILD 契约不变。第三方选择 worker 内首次初始化或 exec 后初始化等合法生命周期。
需要旧的受控 helper 时显式开启 `LOGGER_ENABLE_LEGACY_FORK_HELPER`，详细约束见兼容文档。

## 跨库句柄和配置

借入 logger_t 时，创建/调用/销毁必须使用同一个实际 Logger 实现；不要在多个 SDK 中各自
静态嵌入不同副本，然后跨副本传递 opaque pointer。宿主已有另一套日志系统时，使用 callback ABI。

Logger、Console 的当前顶层配置统一采用 Linux UAPI 风格的
`struct_size/version` 合同。当前已知结构必须完整存在；未来更大的结构只有在未知 tail
全部为 0 时才被旧库接受，未知非零语义返回 `E2BIG`。不支持的 version 返回
`EPROTONOSUPPORT`，过短结构返回 `EINVAL`。pre-v1 zero/zero、旧 prefix 和历史
old-header consumer 均不进入当前兼容合同。

结构体本身在 init/create 期间复制为内部快照；其中的字符串指针只在该调用期间借用，
调用者必须保证其有效且不被并发修改。完整规则见 `docs/UAPI.md`。

当前开发产物使用 SONAME 2 / `LOGGER_2.0`，安装元数据为 `LOGGER_ABI_VERSION=2`；不得冒充 v1 兼容包。

完整集成示例和关闭顺序见 [HOST_OWNED.md](docs/HOST_OWNED.md)。

## 文件所有权加固（当前开发契约）

即使在 NONE/EXTERNAL 模式下，普通文件目标也只能由单个实例拥有。针对同一已绑定目录/基础文件名
创建独立 Logger 时会返回 EBUSY；应改为共享同一个 Logger。后端会一直保留目录文件描述符以及
`<active>.logger.lock` 的持久 `flock` 租约，直到实例完成处置。最终路径分量如果是符号链接或
硬链接会被拒绝；所在目录必须可信。目录及协调文件必须允许必要的访问；
协调文件不存在时由库以0600创建；已存在时必须可写且通过普通单链接文件检查。

时间戳归档文件绝不会被覆盖；内部轮转要求 Linux 支持 `RENAME_NOREPLACE`，否则返回 ENOTSUP。
重新打开会先打开并验证新文件，再淘汰当前工作文件描述符。切换完成后发生 `close` 错误不会恢复
旧文件描述符。错误仍会通过 I/O 状态、刷新和销毁路径对外可见。

边界、资源成本、错误状态，以及进程退出测试与掉电测试之间的区别，见 `docs/FILE_BACKEND.md`。

## Syslog 非阻塞修订

本地数据报 Syslog 现在使用非阻塞、执行时关闭的套接字；发生背压时会形成明确可见的失败记录，
绝不会隐式转化为重放队列。`logger_config_t` 的 `syslog` 字段沿用当前已知布局；pre-v1 短前缀不再受支持。未来扩展遵守 `docs/UAPI.md` 的尾部追加与未知零尾规则。`logger_get_syslog_metrics()` 提供一致的、面向该输出端的计数器和当前状态。
启动策略默认是 REQUIRED；DEFERRED 会显式接受临时端点故障，并记录粘滞错误状态。重连由单调时钟
冷却周期节流，只由后续日志记录触发，不会由刷新/销毁触发。重连成功也绝不会清除实例历史上的
`first_error`。

字段定义、errno、所有权、ABI 限制、本地线格式，以及“有界系统调用尝试”和“实际墙钟延迟”之间的区别，
详见 [SYSLOG_BACKEND.md](docs/SYSLOG_BACKEND.md)。本次修订并未增加远端持久化投递能力，
也不允许在多线程原始 fork 后重建运行时。

## 全局门面代次与取消（本轮更新）

默认 Logger 仍仅用于宿主便利层。所有 global 数据/控制入口统一准入和 generation
复核，旧调用不会操作新实例。`logger_shutdown_status()` 返回最终处置错误；原
`logger_shutdown()` 保留兼容。拒绝状态、启动 fallback、取消延迟和重入边界见
[GLOBAL_LIFECYCLE.md](docs/GLOBAL_LIFECYCLE.md)。显式实例/Console 的作用域规则见下一节。

## 显式实例/Console 的取消与重入加固

当前所有权契约见 `docs/EXPLICIT_CONSOLE_SCOPE.md`。资源操作会将取消动作延后，直到库内部锁和
临时资源全部释放。构造期间如果存在待处理的延迟取消，会处置尚未返回给调用方的候选实例；如果启用了
异步取消，构造阶段直接以 ENOTSUP 拒绝。已经成功返回的裸指针仍由宿主负责管理。Logger/Console
自身回调中再次嵌套调用 Logger/Console 资源操作（即使目标是另一个实例）会以 EDEADLK 拒绝。
正常的 SDK 到宿主适配器属于顶层调用，继续受到支持。函数签名和公开布局均未改变。

## 历史 Audit 与旧 ABI

Audit/crypto 不是当前生产 API，安装库不导出 `audit_*`，也不安装 `audit.h`。
旧审计合同、SHA-256历史和SONAME 1由 `release/1.x` 与 `v1.0.0` 定义，不能从当前普通日志库获得。
当前移除边界与未完成事项见 [AUDIT_REMOVAL.md](docs/v2/AUDIT_REMOVAL.md)。
