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

最低 kernel baseline 固定为 **Linux 5.10**，并在 Ubuntu 20.04 / glibc 2.31 rootfs 的
真实 QEMU guest 中验证当前 production shared runtime：

- guest `uname` 必须为 5.10.x，glibc 必须为 2.31；
- kernel CRNG ready 后实际执行 `getrandom`；
- ext4 与 XFS 的 `RENAME_NOREPLACE` internal rotation；
- Audit 在 ext4/XFS 上的 init/write/shutdown/verify；
- Audit owned-dirfd recovery/checkpoint 路径。

minimum-kernel gate 不再重复 shared/static packaging、完整 consumer、process-crash、
sanitizer 等已有独立 workflow 的证明。更老 kernel 只在出现真实部署需求时建立单独
compatibility tier，不进入通用 Production v1 支持范围。

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

Production v1 的 public config ABI 在切换 SONAME 1 前统一为 Linux UAPI 演进规则：

- Logger / Audit / Console 顶层配置均以 `struct_size/version` 为固定 ABI header；
- pre-v1 zero/zero、旧 prefix、历史 old-header consumer 不进入 v1；
- 当前结构必须完整存在，过短返回 `EINVAL`；
- future larger struct 的未知 tail 全 0 时允许旧库接受；
- unknown tail 任意非 0 返回 `E2BIG`，绝不静默忽略新语义；
- unknown version 返回 `EPROTONOSUPPORT`；
- 新字段只允许尾部追加且零值必须表示未请求新语义，否则提升 version；
- runtime 复制结构体字段为内部快照；字符串指针只在 init/create 调用期间借用。

完整规则由 `docs/UAPI.md` 冻结。

正式 v1 前完成一次 public ABI review，冻结：

- 62 个目标 public symbol 的去留；
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
3. `v1_abi_contract` 冻结 public struct layout、enum/macro 数值、62 个函数类型和默认配置，
   并由 C11/C++11 + release-validation 持续验证。

只有其余 v1 hard gate 关闭并准备正式 1.0 release 时，才把 ABI 1 从 preview 切为默认。

### 5. Audit recovery 容量契约（实现级阻断已关闭）

当前 archive 上限保持 4096。恢复仍会完整扫描并验证 retained log bytes，但扫描完成后的
segment 拓扑连接已经从 O(N²) 全表查找改为临时排序 digest-edge 索引的 O(N log N)；
没有新增 persistent segment ID/index，也没有改变 record/checkpoint 磁盘格式或降低
fail-closed 校验。

现有 4096-archive capacity gate 已覆盖：

- 合法完整链恢复；
- missing middle segment；
- corrupt segment；
- truncated segment；
- 峰值 RSS；
- 三次重复测量与宽松 CI regression guard。

2026-10-05 的相邻 Ubuntu 24.04 托管运行证据中，旧 O(N²) 对照合法恢复中位数为
1411.047 ms；O(N log N) 版本的保守候选观测为 1281.821 ms，中间缺段失败最大值由
163.268 ms 降至 76.832 ms，最大 RSS 由 2344 KiB 增至 2724 KiB。后续最终 head 的
重复运行还观测到 1058.108 ms 合法恢复中位数，但托管 runner 波动不作为目标服务器 SLA。

因此 v1 不再把“segment 连接 O(N²)”视为实现级发布阻断。仍保留以下边界：

- 内容扫描复杂度仍是 O(保留日志字节数)；
- 4096 archive 是当前发布支持上限；
- CI 时间/RSS 阈值是回归保护线，不是目标服务器 SLA；
- persistent segment ID/可信恢复索引仅在新的实测瓶颈出现后再设计，不能替代内容验证。

详细证据见 `validation/AUDIT_RECOVERY_CAPACITY.md`。

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
