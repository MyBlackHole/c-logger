# c-logger 1.0.0 — Production v1

1.0.0 是 c-logger 的首个稳定 Production v1 发布。默认生产 ABI 已冻结为
SONAME `liblogger.so.1`、ELF symbol version `LOGGER_1.0`，默认公开 C 符号集为 62 项。

本版本只支持 Linux / ELF / x86_64 / glibc，最低 userland 为 glibc 2.31，
最低 Linux 内核基线为 5.10。持久文件/Audit 的库级支持范围为受信任本地目录上的 ext4/XFS；
Syslog 只支持本地 Unix-domain datagram。

## 从 0.9.x 升级

0.9.x 属于 pre-v1 candidate，SONAME 0 不构成与 1.0.0 的二进制兼容承诺。

升级到 1.0.0 时：

- 使用 1.0.0 public headers 重新构建调用方；
- shared consumer 链接到 `liblogger.so.1`；
- 不再依赖已删除的四个误导性 fork marker API；
- 不再使用已删除的 `audit_config_t.fsync_each_record`；
- 所有 Logger/Audit/Console 顶层配置必须显式填写当前 `struct_size/version`；
- pre-v1 zero/zero、短 prefix 和历史 old-header config 不再属于支持合同；
- 需要旧 `logger_fork_reinit()` 的场景只能使用显式 opt-in compatibility build。

## 稳定 ABI / UAPI

Production v1 冻结：

- SONAME：`liblogger.so.1`；
- ELF symbol version：`LOGGER_1.0`；
- 62 个默认 public C symbols；
- `abi/logger-1.0.symbols` 唯一符号 allowlist；
- public struct size/alignment/field offset；
- enum 与数值宏；
- public function type；
- ownership/lifetime、错误码、取消/reentry 边界；
- Logger/Audit/Console 默认配置。

配置结构采用 Linux UAPI 风格演进规则：

- `version` 不支持：`EPROTONOSUPPORT`；
- `struct_size` 小于当前已知结构：`EINVAL`；
- 等于当前结构：正常；
- 更大的 future struct：未知 tail 全 0 时接受；
- 未知 tail 有非 0 字节：`E2BIG`。

后续 1.x 只允许兼容性追加；破坏性 ABI 变化必须进入新的 ABI major。

## 资源管理与对象生命周期

teardown 按 Linux 生命周期不变量收口：

- 先关闭准入；
- cooperative stop/wake worker；
- `pthread_join()` 成功后才释放 worker 可访问资源；
- queue/progress/emit 同步对象销毁属于 quiescence proof；
- 无法证明 quiescent 时禁止 final free，安全保留 retired allocation 优先于 UAF；
- 普通 I/O/worker 内部错误在 quiescence 已证明后可以返回错误并安全释放；
- process object census 防 overflow/underflow。

显式 `logger_t *` 继续采用 host-owned / borrower 模型，不在日志热路径加入隐式 refcount。

## Audit

Audit 在 v1 前完成了以下收口：

- 删除重复 writer-lock 协议，统一使用 file-backend ownership；
- recovery/checkpoint 全面使用已持有的 `dirfd + basename`；
- 不依赖 `/proc/self/fd`；
- instance ID 使用 `getrandom(..., GRND_NONBLOCK)`；
- CRNG 未 ready 时 `audit_init()` fail-fast 返回 `EAGAIN`；
- 不回退弱随机、不在库初始化中无界等待；
- 4096 archive 恢复的 segment 拓扑连接由 O(N²) 改为 O(N log N)；
- 保留完整内容扫描与 fail-closed chain 校验。

Audit 仍只使用 builtin SHA-256。无外部可信签名/锚点/WORM 认证，本地无密钥 hash chain
不能排除整体重算、截尾或整体回滚。

## 平台与验证

Production v1 固定支持：

- Linux；
- ELF64 x86_64；
- glibc >= 2.31；
- Linux >= 5.10；
- shared/static production artifacts；
- 本地 ext4/XFS；
- 本地 Unix-domain datagram Syslog。

持续门禁包括：

- Ubuntu 20.04 / 22.04 / 24.04 glibc runtime matrix；
- Linux 5.10 + glibc 2.31 minimum-kernel QEMU；
- ext4/XFS `RENAME_NOREPLACE`；
- real local syslogd integration；
- ext4/XFS QEMU/raw-disk power-cut cut-point matrix；
- focused process-crash recovery；
- ASan + UBSan；
- TSan；
- 4096 archive recovery capacity；
- installed C/C++/CMake/pkg-config consumers；
- ELF/SONAME/symbol-version/62-symbol ABI checks；
- XPack + SHA-256 release asset validation。

物理 PDU/BMC、RAID/HBA/NVMe/SATA controller、drive firmware 和 volatile write-cache
行为属于特定部署 qualification，不属于通用 c-logger 1.0 的硬件认证声明。

## 明确不支持

1.0.0 不支持：

- 32-bit；
- 非 x86_64；
- musl/非 glibc；
- 非 Linux；
- NFS/SMB/其他网络或分布式文件系统；
- signal-handler-safe 使用；
- hard real-time guarantee；
- `PTHREAD_CANCEL_ASYNCHRONOUS + ENABLE` 进入普通 Logger/Console API；
- `pthread_exit` / `longjmp` 穿过库调用；
- remote durable syslog delivery；
- 多线程 raw-fork 后复用继承的复杂 runtime；
- FIPS、GM/T 等密码模块认证；
- WORM、外部可信签名或远端可信锚点。

## 发布制品

权威发布制品：

- `prod-c-logger-1.0.0-Linux-x86_64-shared.tar.gz`
- `prod-c-logger-1.0.0-Linux-x86_64-static.tar.gz`
- 对应两个 `.sha256`

GitHub Release 必须由 exact release commit/tag 重建并验证，release title 为
`c-logger 1.0.0 — Production v1`，且不是 prerelease。

详细支持边界见：

- `docs/V1_RELEASE_CRITERIA.md`
- `docs/KNOWN_ISSUES.md`
- `docs/RELEASE_ENGINEERING.md`
- `docs/UAPI.md`
- `abi/LOGGER_1_0_CONTRACT.md`
