# 当前 API 与集成边界

本轮未扩写公开配置/metrics 结构，没有改变旧函数签名。新增加 `logger_destroy_status()`；
`logger_fork_reinit()` 的默认可用性有明确变化：仅 opt-in 兼容产物提供。

## 显式实例（第三方默认入口）

- `logger_create()` 返回宿主拥有的实例；失败 NULL + errno。
- `logger_log()`、`logger_log_source()`、`LOGGER_LOG/INFO/...` 使用指定实例。
- `logger_flush_instance_status()` 返回 0 / -1 + errno，等待既定完成水位并同步文件。
  不代表远端 syslog 已持久化，也不提供阻塞 I/O 超时。
- `logger_destroy_status()` 是独占 owner 的关闭/释放接口；调用前先停止并 join 所有使用者。
  排空、join、同步、关闭后端后释放内存；普通 I/O 失败仍释放实例，返回 -1 + errno，
  不能重试旧指针。NULL 在原进程中成功；ECHILD 拒绝继承状态，不碰旧资源。
  极少发生的 pthread cancellation-state 设置失败也在处置前返回。
- `logger_destroy()`、`logger_flush_instance()` 保留为丢弃状态的兼容 wrapper。
- `logger_log_sync_status()` 保留历史 `0/-errno` 约定，不能和 POSIX 风格状态接口混淆。
- Live metrics 是并发近似快照，不是单笔业务提交证明。

生命周期中不得在仍有 caller/worker 使用句柄时 free、重建或卸载库。
不增加 init 引用计数，不隐式夺取借用句柄所有权。

## 普通日志字符串寿命

调用者的 fmt/source/context 输入必须在对应调用期间有效，不得并发修改。
用户消息仍通过 vsnprintf 复制；异步记录新增加 module/source basename/function 的有界快照，
入队和出队时正确绑定自己的缓冲区，不再保留业务 DSO 的 source 指针。
容量（不含 NUL）：module 127、basename 255、function 127 字节。
超限在末尾以 `~` 标识缩短，按字节而不是 Unicode 代码点处理；这是有意的可见边界变化。
仅作为元数据标签，不能拿被缩短的字符串作业务标识或安全判据。

不新增每条日志的堆分配。私有 record 增加 512 字节；内部输出行缓冲变为 5120 字节，
容量 >=2 时保留 LF+NUL，0/1 容量也有界处理。正常长度的原有输出字节保持不变。
运行时变更 TZ 的缓存语义未在本轮设计。

## 全局、Audit、Console

全局 `logger_init/LOG_*/logger_shutdown` 保留便利用法；业务 `.so` 不应偷偷调用。
全局生命周期遵循 docs/GLOBAL_LIFECYCLE.md；显式实例仍由宿主独占销毁。
Audit 保留现有严格生命周期、crypto failure、checkpoint、解析/恢复和 single-writer 契约；
Console 是应用终端展示工具，不是业务 SDK 隐式输出通道。

## Fork 与兼容

默认库保留 PID/atfork 误用防护，但不替宿主 fork 或检查全进程线程列表。
一般 raw fork 的 ECHILD 契约不变。第三方选择 worker 内首次初始化或 exec 后初始化等合法生命周期。
需要旧的受控 helper 时显式开启 `LOGGER_ENABLE_LEGACY_FORK_HELPER`，详细约束见兼容文档。

## 跨库句柄和配置

借入 logger_t 时，创建/调用/销毁必须使用同一个实际 Logger 实现；不要在多个 SDK 中各自
静态嵌入不同副本，然后跨副本传递 opaque pointer。宿主已有另一套日志系统时，使用 callback ABI。
本轮未承诺旧配置二进制 ABI 自适应；结构体 size/version 的既有校验保持不变。
源码/二进制发布 ABI 冻结、symbol visibility、install/export 仍需后续处理。

完整集成示例和关闭顺序见 [HOST_OWNED.md](docs/HOST_OWNED.md)。

## File ownership hardening (current development contract)

Regular-file targets are single-owner even in NONE/EXTERNAL modes. Independent
loggers for the same bound directory/basename return EBUSY; share one logger
instead. The backend retains a directory fd and a persistent
`<active>.logger.lock` flock lease through disposal. Final-component symlinks
and hardlinks are rejected; the directory must be trusted. A writable/accessibly
precreated coordination file is now required, a compatibility change.

Timestamp archives are never overwritten; internal rotation requires Linux
RENAME_NOREPLACE support, otherwise returns ENOTSUP. Reopen opens and validates
before retiring the working fd. A post-switch close error does not restore the
old fd. Errors remain visible through I/O status/flush/destroy.

Audit reserves active and checkpoint ownership before recovery, and anchors its
path-based recovery/checkpoint operations via owned `/proc/self/fd` directory
paths (requires accessible procfs). No public function signatures or config/
checkpoint/record layouts change. See `docs/FILE_BACKEND.md` for boundaries,
resource cost, error states, and the difference between process death and power
loss testing.

## Syslog nonblocking revision

Local datagram Syslog now uses a nonblocking close-on-exec socket; backpressure
is a visible failed record, never an implicit replay queue. `logger_config_t`
appends `syslog` (old version-1 prefixes are read boundedly with defaults).
`logger_get_syslog_metrics()` exposes coherent sink-specific counters/current
state. Startup is REQUIRED by default; DEFERRED explicitly accepts transient
endpoint failure and records sticky error state. Reconnect is paced by a
monotonic cooldown and triggered only by future records, not by flush/destroy.
Successful reconnect never clears the instance's historical first_error.

See [SYSLOG_BACKEND.md](docs/SYSLOG_BACKEND.md) for exact fields, errno, ownership,
ABI limits, local wire format, and the distinction between bounded syscall
attempts and wall-clock latency. This does not add remote durable delivery or
permit multi-thread raw-fork runtime reconstruction.

## Global facade generation 与取消（本轮更新）

默认 Logger 仍仅用于宿主便利层。所有 global 数据/控制入口统一准入和 generation
复核，旧调用不会操作新实例。`logger_shutdown_status()` 返回最终处置错误；原
`logger_shutdown()` 保留兼容。拒绝状态、启动 fallback、取消延迟和重入边界见
[GLOBAL_LIFECYCLE.md](docs/GLOBAL_LIFECYCLE.md)。显式实例的全入口取消不在此保证内。

## Explicit/Console cancellation and reentry hardening

See `docs/EXPLICIT_CONSOLE_SCOPE.md` for the current ownership contract. Resource
operations defer cancellation until library locks and temporary resources are
released. Constructor pending deferred cancellation disposes the unreturned
candidate; enabled asynchronous cancellation is rejected at construction with
ENOTSUP. The host remains responsible for a successfully returned raw pointer.
Nested Logger/Console resource calls from their own callbacks, including another
instance, are rejected with EDEADLK. Normal SDK-to-host adapters are top-level
calls and remain supported. No function signature or public layout changed.

## 内置 SHA-256（当前契约）

只保留 `AUDIT_INTEGRITY_SHA256 = 1`，它仍是默认完整性配置。不再提供 SM3 实现或
`AUDIT_INTEGRITY_SM3` 枚举常量。`AUDIT_INTEGRITY_NONE = 0` 保留为显式关闭完整性的
现有开关，不是另一个算法，也绝不在计算错误时自动启用。

公有函数签名和结构布局不变；删除 SM3 常量是明确的源码兼容性收紧，不宣称没有 API
变化。旧二进制传入原编号 2 或其他不支持的算法，init/with-verifier 返回
`EPROTONOSUPPORT`，不得映射到 SHA-256。已活跃/继承/重入等现有状态检查优先级不变。
私有 checkpoint parser 保留既有错误约定：不支持的算法号属于不可接收的 checkpoint，
load 返回 `EBADMSG` 且输出不变。数字 2 永久保留，不重新分配。

`audit_crypto_backend()` 为兼容诊断保留，固定返回静态字符串 `"builtin"`。
`audit_verify_file()` / `audit_verify_file_from()` 默认只验证 SHA-256；保留
`audit_verify_file_with()` 作为兼容入口，只接受 SHA-256，NONE 也不是验证算法。

有效的旧 SHA-256 链及 checkpoint v2 原样验证、恢复、接续，无需重算历史。
SM3 历史不再支持：保留归档与对应旧验证工具/版本离线核查，为新 SHA-256 链选择独立
目标。不得把 alg=2 改成 alg=1，或将 SM3 末尾 hash 当成新链的可信锚点；本版不做这种
自动迁移。纯日志行本身不含算法 ID：显式请求原 ID 2 在读取前被拒绝，用默认 SHA-256
核验真实 SM3 内容则返回 `EBADMSG`。SHA-256 校验不是算法自动识别。

Fail-closed 的返回值、输出不变契约和 `CRYPTO_FAILED` 均保留。见
[BUILTIN_CRYPTO.md](docs/BUILTIN_CRYPTO.md)。

## Packaged 0.9.0 ABI snapshot

See docs/RELEASE_ENGINEERING.md. Public layouts and signatures are unchanged; LOGGER_API annotates the Linux shared ABI. All undocumented implementation names are hidden. C++11 convenience defaults are header-only and retain C defaults/layouts. Package version 0.9.0 matches exactly; candidate SONAME 0 is not a Production v1 guarantee.
