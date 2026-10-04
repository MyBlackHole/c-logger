# Production v1 支持范围与发布准入

本文定义 c-logger **Production v1** 的目标支持范围和发布硬门禁。它不是对当前 0.9.6
candidate 的扩大承诺；只有“已冻结支持范围”中的条目满足对应 v1 证据后，才能在正式 v1
发布中声明支持。

跟踪入口：GitHub issue #73（Production release-hardening roadmap），子门禁为 #68–#72。

核心原则：

1. **支持范围必须可验证**，不能从 syscall 首次出现版本或构建成功反推兼容性；
2. **明确不支持**优于保留模糊的“尚未验收”；
3. v1 只冻结真实需要长期维护的最小平台集合；
4. CI、QEMU 与静态 ELF 检查是证据的一部分，不替代最低平台和真实存储栈验收。

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

因此 v1 的 **userland baseline 固定为 glibc >= 2.31**。这只描述 libc/userland，不代表最低
Linux kernel 已经确定。最低 kernel 是独立的 v1 blocker。

### 文件系统与目录信任模型

v1 的持久文件/Audit 支持范围限定为：

- 本地 ext4；
- 本地 XFS；
- 受信任的本地目录；
- 单目标 cooperative owner；
- 支持 `RENAME_NOREPLACE` 的实际 kernel/filesystem 组合；
- Audit 使用场景必须保持可访问 procfs，使 owned procfd 路径绑定语义成立。

这里的“支持 ext4/XFS”要求正式 v1 前补齐真实目标存储栈证据。现有 QEMU/raw-disk ext4 +
XFS × 10 cut-point matrix 是必要 CI gate，但不是物理断电认证。

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
- 初始化后进入导致 procfs 不可访问的 chroot / namespace / procfs unmount 场景；
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

以下项目全部关闭前，不得把 release title / README / package metadata 改为 Production v1。

### 1. 最低 Linux kernel

必须选择一个明确的最低 kernel baseline，并在**实际该 kernel 或等价可追溯 VM/target**上验证：

- shared/static production artifact；
- production-linked core tests；
- installed C/C++ consumer；
- pkg-config consumer；
- `getrandom` 路径；
- `RENAME_NOREPLACE` 路径；
- procfs/procfd Audit 路径；
- process crash recovery。

不能用“某 syscall 在 Linux X.Y 首次出现”代替运行验证，也不能用共享 runner kernel 的
container matrix 宣称最低 kernel。

最低 kernel 在证据完成前保持 **TBD**。

### 2. 真实存储栈掉电验证

至少对正式支持的 ext4 和 XFS 各完成一套可追溯的真实目标存储栈验收，记录：

- server / VM host 型号或环境；
- kernel；
- filesystem 与 mount options；
- block device / RAID / HBA / NVMe/SATA controller；
- volatile write-cache 策略；
- c-logger commit / artifact checksum；
- cut point；
- reboot/recovery/append/verify 结果。

重点覆盖与 CI 一致的 acknowledged、Audit fsync/checkpoint 与 file rotation switch 边界。
如果某一 filesystem 无法取得真实存储栈证据，应从 v1 支持范围中删除，而不是保留模糊承诺。

### 3. 真实 syslogd integration

在最低 userland / kernel 目标上使用真实 local syslog daemon 完成：

- startup required/deferred；
- endpoint unavailable；
- reconnect cooldown；
- backpressure；
- daemon restart；
- shutdown/flush 边界；
- metrics/diagnostics 可观察性。

通过 fake socket/unit test 不能独立关闭此 gate。

### 4. v1 public ABI freeze

正式 v1 前完成一次 public ABI review，冻结：

- 66 个现有 public symbol 的去留；
- public struct layout；
- enum 数值；
- ownership/lifetime；
- error convention；
- cancellation/reentry boundary；
- global facade generation semantics；
- Audit record/checkpoint compatibility boundary。

v1 发布时应把 ABI identity 从 candidate 契约切到稳定契约。当前目标是：

- SONAME：`liblogger.so.1`；
- ELF symbol version：`LOGGER_1.0`；
- 建立 `abi/logger-1.0.symbols` 或等价冻结 snapshot；
- 后续 1.x 只允许兼容性 additive ABI 变化，破坏性变化进入下一个 ABI major。

具体迁移必须由独立 PR 完成并同时验证 old/new consumer，不在本文档中直接修改二进制 ABI。

当前 hardening 分阶段完成：

1. 已冻结 `abi/logger-1.0.symbols`，CI 阻止 active public symbol surface 未审查漂移；
2. 非默认 `v1_abi_preview` 已验证 SONAME 1 / `LOGGER_1.0` / installed metadata，
   同时保持当前 0.9.6 默认产物为 ABI 0；
3. `v1_abi_contract` 冻结 public struct layout、enum/macro 数值、66 个函数类型和默认配置，
   并由 C11/C++11 + release-validation 持续验证。

只有其余 v1 hard gate 关闭并准备正式 1.0 release 时，才把 ABI 1 从 preview 切为默认。

### 5. Audit recovery 容量契约

当前 recovery 对 retained segment 扫描/连接存在 O(N²) 路径，archive 上限为 4096。

v1 前必须：

- 构造 4096 archive worst-case recovery 数据集；
- 测量恢复时间与峰值资源；
- 验证 corrupt/truncated/missing segment 场景；
- 明确发布支持上限和可接受恢复时间边界。

如果结果不可接受，再决定优化 persistent segment ID/index；不能在没有数据时先做大规模重写。

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

- 本文所有 hard gate 关闭；
- `docs/KNOWN_ISSUES.md` 中剩余条目均被分类为明确 unsupported、documented limitation 或
  post-v1 enhancement，而不是未知兼容性；
- v1 ABI snapshot 已冻结并通过 packaging/consumer gate；
- release notes 明确列出 supported/unsupported platform contract；
- release artifact 与证据能追溯到同一个 tag/commit/checksum。

在这些条件满足前，版本仍应保持 controlled production candidate，不得仅因测试数量很多而
提前改称 Production v1。
