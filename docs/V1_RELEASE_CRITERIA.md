# 生产版 v1 支持范围与发布准入

本文定义 c-logger **生产版 v1** 的目标支持范围和发布硬门禁。它不是对当前 0.9.6
候选版本 的扩大承诺；只有“已冻结支持范围”中的条目满足对应 v1 证据后，才能在正式 v1
发布中声明支持。

跟踪入口：GitHub issue #73（生产 发布加固路线图），子门禁为 #68–#72。

核心原则：

1. **支持范围必须可验证**，不能从 syscall 首次出现版本或构建成功反推兼容性；
2. **明确不支持**优于保留模糊的“尚未验收”；
3. v1 只冻结真实需要长期维护的最小平台集合；
4. CI、QEMU 与静态 ELF 检查是证据的一部分，不替代最低平台和真实存储栈验收。

## 已冻结的 v1 支持范围

### 操作系统 / 架构 / ABI

v1 只支持：

- Linux；
- ELF；
- x86_64 / ELF64；
- glibc 用户态环境；
- 共享库与静态库 两种生产制品；
- C 公开 ABI，C++ 只作为 C 头文件 / CMake 消费方 兼容面。

当前已执行的 glibc 运行时矩阵 覆盖：

- Ubuntu 20.04 / glibc 2.31；
- Ubuntu 22.04 / glibc 2.35；
- Ubuntu 24.04 / glibc 2.39。

因此 v1 的 **用户态基线 固定为 glibc >= 2.31**。这只描述 libc/用户态环境，不代表最低
Linux kernel 已经确定。最低 kernel 是独立的 v1 阻断项。

### 文件系统与目录信任模型

v1 的持久文件/Audit 支持范围限定为：

- 本地 ext4；
- 本地 XFS；
- 受信任的本地目录；
- 单目标 协作式所有者；
- 支持 `RENAME_NOREPLACE` 的实际 内核/文件系统 组合；
- Audit 使用场景必须保持可访问 procfs，使 owned procfd 路径绑定语义成立。

这里的“支持 ext4/XFS”要求正式 v1 前补齐真实目标存储栈证据。现有 QEMU/raw-disk ext4 +
XFS × 10 切断点矩阵 是必要 CI 门禁，但不是物理断电认证。

### Syslog

v1 只支持当前实现的：

- 本地 Unix 域数据报 Syslog；
- 非阻塞发送；
- 显式 背压/故障记账；
- 由后续日志触发的 有界重连。

v1 **不承诺**远端持久确认、失败消息重放、磁盘重试队列或 端到端持久投递。

### 集成与生命周期

v1 推荐并长期支持：

- 宿主持有的显式 `logger_t`；
- SDK 借用实例或 宿主回调；
- stop/join 借用者 → flush → destroy → un加载 的关闭顺序。

全局门面 继续作为宿主便利 API，但不改变显式 owner 模型。

默认生产包**不包含** 旧版 `logger_fork_reinit()`。继承活跃运行时后的 原始 fork 使用仍拒绝；
兼容 helper 仅属于 显式启用 兼容构建，不进入 v1 默认契约。

### 构建源码的工具链范围

从源码构建 v1 时要求：

- GNU C11（`-std=gnu11`）；
- GCC/Clang 兼容 GNU C extensions；
- GNU 兼容 ELF 链接器 与 version-script 支持；
- Xmake >= 2.8.5；CI 固定版本由 工作流 明确记录。

内部 GNU 扩展不进入 已安装公开头文件 ABI。

## v1 明确不支持

下列场景不属于 v1 支持范围，不能作为发布阻断项继续无限扩张：

- 32-bit；
- 非 x86_64 架构；
- musl 或其他非 glibc libc；
- macOS、BSD、Windows；
- NFS、SMB 以及其他网络/分布式文件系统；
- 除 ext4/XFS 外未明确验收的文件系统；
- 恶意目录替换、恶意/不协作 写入器、锁 inode 删除替换等把 cooperative lock 当安全边界的场景；
- 初始化后进入导致 procfs 不可访问的 chroot / namespace / procfs unmount 场景；
- 信号处理器安全 使用；
- `PTHREAD_CANCEL_ASYNCHRONOUS + ENABLE` 进入普通 Logger/Console API；
- `pthread_exit` / `longjmp` 穿过库调用；
- 硬实时延迟保证；
- 远端 Syslog 持久投递；
- 多线程 raw-fork 后重建任意第三方库运行时；
- FIPS、GM/T 或其他密码模块/安全认证；
- 外部可信签名、远端锚点、WORM 合规；
- 通过本地无密钥 SHA-256 链 排除“整体重算、整体回滚或整段删除”。

这些边界必须在 README / API / KNOWN_ISSUES 中保持一致，不得把“不支持”重新写成
“可能支持但尚未测试”。

## 生产版 v1 硬门禁

以下项目全部关闭前，不得把 发布标题 / README / 软件包元数据 改为 生产版 v1。

### 1. 最低 Linux kernel

必须选择一个明确的最低 内核基线，并在**实际该 kernel 或等价可追溯 VM/target**上验证：

- 共享库/静态库 生产产物；
- 链接生产库的核心测试；
- 已安装 C/C++ 消费方；
- pkg-config 消费方；
- `getrandom` 路径；
- `RENAME_NOREPLACE` 路径；
- procfs/procfd Audit 路径；
- 进程崩溃恢复。

不能用“某 syscall 在 Linux X.Y 首次出现”代替运行验证，也不能用共享 运行器内核 的
容器矩阵 宣称最低 kernel。

最低 kernel 在证据完成前保持 **TBD**。

### 2. 真实存储栈掉电验证

至少对正式支持的 ext4 和 XFS 各完成一套可追溯的真实目标存储栈验收，记录：

- 服务器 / VM 宿主 型号或环境；
- kernel；
- 文件系统 与 挂载选项；
- 块设备 / RAID / HBA / NVMe/SATA 控制器；
- 易失写缓存 策略；
- c-logger commit / 产物校验和；
- cut point；
- 重启/恢复/追加/验证 结果。

重点覆盖与 CI 一致的 已确认、Audit fsync/checkpoint 与 file 轮转 switch 边界。
如果某一 文件系统 无法取得真实存储栈证据，应从 v1 支持范围中删除，而不是保留模糊承诺。

### 3. 真实 syslogd integration

在最低 用户态环境 / kernel 目标上使用真实 本地 Syslog 守护进程 完成：

- 启动 REQUIRED/DEFERRED；
- 端点不可用；
- 重连冷却；
- back压力；
- 守护进程重启；
- shutdown/flush 边界；
- 指标/诊断 可观察性。

通过 模拟套接字/单元测试 不能独立关闭此 gate。

### 4. v1 公开 ABI 冻结

正式 v1 前完成一次 公开 ABI 评审，冻结：

- 66 个现有 公开 symbol 的去留；
- 公开结构布局；
- enum 数值；
- 所有权/生命周期；
- 错误约定；
- 取消/重入边界；
- 全局门面代次语义；
- Audit record/checkpoint compatibility boundary。

v1 发布时应把 ABI identity 从 候选版本 契约切到稳定契约。当前目标是：

- SONAME：`liblogger.so.1`；
- ELF symbol version：`LOGGER_1.0`；
- 建立 `abi/logger-1.0.symbols` 或等价冻结 快照；
- 后续 1.x 只允许兼容性 增量 ABI 变化，破坏性变化进入下一个 ABI 主版本。

具体迁移必须由独立 PR 完成并同时验证 旧/新消费方，不在本文档中直接修改二进制 ABI。

当前 加固 分阶段完成：

1. 已冻结 `abi/logger-1.0.symbols`，CI 阻止 当前公开符号接口面 未审查漂移；
2. 非默认 `v1_abi_preview` 已验证 SONAME 1 / `LOGGER_1.0` / 已安装元数据，
   同时保持当前 0.9.6 默认产物为 ABI 0；
3. `v1_abi_contract` 冻结 公开结构布局、enum/macro 数值、66 个函数类型和默认配置，
   并由 C11/C++11 + 发布验证 持续验证。

只有其余 v1 硬门禁 关闭并准备正式 1.0 发布 时，才把 ABI 1 从 预览 切为默认。

### 5. Audit 恢复 容量契约

当前 恢复 对 保留分段 扫描/连接存在 O(N²) 路径，归档 上限为 4096。

v1 前必须：

- 构造 4096 归档 最坏情况恢复 数据集；
- 测量恢复时间与峰值资源；
- 验证 损坏/截断/缺失分段 场景；
- 明确发布支持上限和可接受恢复时间边界。

如果结果不可接受，再决定优化 持久分段 ID/索引；不能在没有数据时先做大规模重写。

## 已完成且继续保持的门禁

以下不是剩余 阻断项，但 v1 不得回退：

- Apache License 2.0 随源码与 XPack/安装树 分发；
- 隐藏可见性；
- 显式公开符号允许列表；
- 生产 故障钩子 隔离；
- 共享库/静态库 发布 validation；
- installed C/C++/pkg-config 消费方；
- ASan + UBSan；
- TSan；
- process-crash matrix；
- ext4 + XFS QEMU/raw-disk 断电 matrix；
- glibc 2.31/2.35/2.39 运行时矩阵；
- 队列基准测试；
- SHA-256 发布资产验证；
- 发布/恢复 幂等。

## v1 发布判定

生产版 v1 只能在以下条件同时成立时发布：

- 本文所有 硬门禁 关闭；
- `docs/KNOWN_ISSUES.md` 中剩余条目均被分类为明确 不支持、已记录限制 或
  v1 后增强项，而不是未知兼容性；
- v1 ABI 快照 已冻结并通过 packaging/consumer gate；
- 发布 notes 明确列出 支持/不支持平台契约；
- 发布产物 与证据能追溯到同一个 tag/commit/校验和。

在这些条件满足前，版本仍应保持 受控生产候选版本，不得仅因测试数量很多而
提前改称 生产版 v1。
