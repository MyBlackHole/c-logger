# Production v1 支持范围与发布准入

本文定义 c-logger **Production v1** 的目标支持范围和发布硬门禁。它不是对当前 0.9.6
candidate 的扩大承诺；只有“已冻结支持范围”中的条目满足对应 v1 证据后，才能在正式 v1
发布中声明支持。

跟踪入口：GitHub issue #73（Production release-hardening roadmap），子门禁为 #68–#72。

核心原则：

1. **支持范围必须可验证**，不能从 syscall 首次出现版本或构建成功反推兼容性；
2. **明确不支持**优于保留模糊的“尚未验收”；
3. v1 只冻结真实需要长期维护的最小平台集合；
4. CI、QEMU 与静态 ELF 检查证明库级合同；特定服务器/控制器/介质的物理掉电能力属于部署 qualification，不作为通用库 v1 发布门禁。

## 已冻结的 v1 支持范围

### OS / architecture / ABI

v1 只支持：

- Linux；
- ELF；
- x86_64 / ELF64；
- glibc userland；
- shared 与 static 两种生产制品；
- C public ABI，C++ 只作为 C header / CMake consumer 兼容面。

当前已执行的 glibc runtime matrix 覆盖：

- Ubuntu 20.04 / glibc 2.31；
- Ubuntu 22.04 / glibc 2.35；
- Ubuntu 24.04 / glibc 2.39。

因此 v1 的 **userland baseline 固定为 glibc >= 2.31**，最低 upstream kernel baseline
固定为 **Linux >= 5.10 / x86_64**。5.10 作为仍受 upstream longterm 维护的老 LTS，
避免为了理论最老 syscall 版本长期维护 3.x/4.x 的文件系统和早期启动特例。

### 文件系统与目录信任模型

v1 的持久文件/Audit 支持范围限定为：

- 本地 ext4；
- 本地 XFS；
- 受信任的本地目录；
- 单目标 cooperative owner；
- Linux 5.10+ 上 ext4/XFS 的 `RENAME_NOREPLACE` 路径；
- Audit recovery/checkpoint 使用已持有的目录 fd；procfs 不属于运行时依赖。

这里的“支持 ext4/XFS”指库级文件系统语义已由 Linux 5.10 minimum-kernel 与 QEMU/raw-disk
ext4/XFS cut-point matrix 验证。该证据不等于任意服务器、RAID/HBA/NVMe/SATA write-cache
组合的物理掉电认证；需要这类保证的产品/部署必须单独 qualification。

### Syslog

v1 只支持当前实现的：

- local Unix-domain datagram syslog；
- nonblocking send；
- 显式 backpressure/failure accounting；
- 由后续日志触发的 bounded reconnect。

v1 **不承诺**远端持久确认、失败消息重放、磁盘重试队列或 end-to-end durable delivery。

### 集成与生命周期

v1 推荐并长期支持：

- host-owned explicit `logger_t`；
- SDK 借用实例或 host callback；
- stop/join borrowers → flush → destroy → unload 的关闭顺序。

Global facade 继续作为宿主便利 API，但不改变显式 owner 模型。

默认生产包**不包含** legacy `logger_fork_reinit()`。继承活跃运行时后的 raw fork 使用仍拒绝；
兼容 helper 仅属于 opt-in compatibility build，不进入 v1 默认契约。

### 构建源码的工具链范围

从源码构建 v1 时要求：

- GNU C11（`-std=gnu11`）；
- GCC/Clang 兼容 GNU C extensions；
- GNU-compatible ELF linker 与 version-script 支持；
- Xmake >= 2.8.5；CI 固定版本由 workflow 明确记录。

内部 GNU 扩展不进入 installed public header ABI。

## v1 明确不支持

下列场景不属于 v1 支持范围，不能作为发布阻断项继续无限扩张：

- 32-bit；
- 非 x86_64 architecture；
- musl 或其他非 glibc libc；
- macOS、BSD、Windows；
- NFS、SMB 以及其他网络/分布式文件系统；
- 除 ext4/XFS 外未明确验收的文件系统；
- 恶意目录替换、恶意/不协作 writer、锁 inode 删除替换等把 cooperative lock 当安全边界的场景；
- signal-handler-safe 使用；
- `PTHREAD_CANCEL_ASYNCHRONOUS + ENABLE` 进入普通 Logger/Console API；
- `pthread_exit` / `longjmp` 穿过库调用；
- hard real-time latency guarantee；
- remote syslog durable delivery；
- 多线程 raw-fork 后重建任意第三方库运行时；
- FIPS、GM/T 或其他密码模块/安全认证；
- 外部可信签名、远端锚点、WORM 合规；
- 通过本地无密钥 SHA-256 chain 排除“整体重算、整体回滚或整段删除”。

这些边界必须在 README / API / KNOWN_ISSUES 中保持一致，不得把“unsupported”重新写成
“可能支持但尚未测试”。

## Production v1 硬门禁

实现级 Production v1 hard gate 已全部关闭。以下四项必须持续保持绿色，任何回退都重新打开发布阻断：

### 1. 最低 Linux kernel（已关闭）

最低 baseline 固定为 **Linux 5.10 / x86_64 + glibc 2.31**。minimum-kernel gate 已在真实
QEMU 5.10 guest 中验证 getrandom、ext4/XFS `RENAME_NOREPLACE`、Audit
write/recovery/checkpoint owned-dirfd 路径。更老内核不进入通用 v1 支持合同。

### 2. 真实 local syslogd integration（已关闭）

真实 local syslog daemon gate 已覆盖 required/deferred startup、endpoint unavailable、
reconnect cooldown、backpressure、daemon restart、shutdown/flush 与 metrics/diagnostics。
这不扩展为 remote durable delivery。

### 3. v1 public ABI freeze（已关闭）

默认 production ABI 已冻结为：

- SONAME `liblogger.so.1`；
- ELF symbol version `LOGGER_1.0`；
- 62-symbol `abi/logger-1.0.symbols` 唯一 allowlist；
- installed `LOGGER_ABI_VERSION=1`；
- `v1_abi_contract` 冻结 public layout、enum/macro、函数类型和默认配置；
- Logger/Audit/Console 配置遵守 `docs/UAPI.md` 的 Linux UAPI 演进规则。

后续 1.x 只允许兼容性 additive ABI 变化；破坏性变化进入新的 ABI major。

### 4. Audit recovery 容量契约（已关闭）

4096 archive 上限保持不变。segment 拓扑连接已由 O(N²) 收敛为 O(N log N)，capacity gate
覆盖合法完整链、缺段、损坏、截断、峰值 RSS 与重复测量。恢复仍完整扫描并验证保留日志字节；
持久索引属于 post-v1 enhancement，不能替代内容验证。

### 部署 qualification，不是通用发布门禁

物理断电/PDU/BMC、RAID/HBA/NVMe/SATA controller、drive firmware、volatile write-cache
策略属于特定部署的 durability qualification。仓库继续要求 deterministic QEMU/raw-disk
ext4/XFS cut-point matrix 作为库级 crash-consistency gate，但不把任意硬件组合认证扩大成
通用 c-logger Production v1 发布条件。跟踪模板见 issue #69。

## 已完成且继续保持的门禁

以下不是剩余 blocker，但 v1 不得回退：

- Apache License 2.0 随源码与 XPack/install tree 分发；
- hidden visibility；
- explicit public symbol allowlist；
- production fault hooks 隔离；
- shared/static release validation；
- installed C/C++/pkg-config consumer；
- ASan + UBSan；
- TSan；
- process-crash matrix；
- ext4 + XFS QEMU/raw-disk power-cut matrix；
- glibc 2.31/2.35/2.39 runtime matrix；
- queue benchmark；
- SHA-256 release asset verification；
- release publish/recovery 幂等。

## v1 发布判定

Production v1 只能在以下条件同时成立时发布：

- 上述实现级 hard gate 全部保持关闭；
- `docs/KNOWN_ISSUES.md` 中剩余条目均被分类为明确 unsupported、documented limitation 或
  post-v1 enhancement，而不是未知兼容性；
- v1 ABI snapshot 已冻结并通过 packaging/consumer gate；
- release notes 明确列出 supported/unsupported platform contract；
- release artifact 与证据能追溯到同一个 tag/commit/checksum。

当前实现级 hard gate 已关闭。正式切换 `VERSION=1.0.0` 前仍必须完成最终文档一致性、
release notes、exact release commit/tag/checksum 审查；这些发布动作完成前继续保持
controlled production candidate。
