# 尚未关闭的发布阻断项（仅内置 SHA-256）

本项目当前发布仍不是 Production v1。v1 的支持范围、明确 不支持 范围和硬门禁已经集中定义在
[V1_RELEASE_CRITERIA.md](V1_RELEASE_CRITERIA.md)。本表继续记录实现边界与待办；属于 v1 明确
不支持 的场景不再作为发布阻断项。旧轮次报告保留历史证据，不表示旧用法仍受支持。

## 已完成的定向修复

- Queue/Flush：等待通知协议、完成水位、粘滞 I/O 错误 与状态 flush。
- Audit 生命周期：实例资源归属、START/STOP 准入、检查点 失败暂停和 I/O 不确定结果停写。
- Crypto：可返回错误的摘要 provider、CRYPTO_FAILED、历史 SM3 移位 UB 已修；当前 SM3 已按需求删除，不背书旧可疑历史。
- 解析/恢复：严格 KV 和有界行读取、EOF 残片证据、按已验证 hash 边连接保留集合。
- Fork/测试隔离：共享继承状态防护，默认生产库没有故障/退出钩子与扫描/创建进程 辅助接口。
- 宿主持有：SDK 日志回调、实例借用、异步 源信息 有界快照、严格处置、实际 DSO 集成。
- 文件后端：受信任本地目录下单目标 所有者、固定 dirfd、无覆盖时间轮换、重新打开 保留旧 fd、资源错误清理；Audit 在恢复之前取得 active/state 所有权。
- Syslog：独立配置有界复制、非阻塞输出、显式背压、仅后续记录触发的有界重连、sink 指标和旧 version-1 config 前缀读取。
- Global：统一 ticket（generation + phase）准入，读侧/延迟 shutdown 复核，控制 reader 关闭门禁；取消延迟到释放 pin/锁之后，同线程嵌套拒绝；新增状态 shutdown。没有改变宿主拥有显式实例的推荐模型。

- Explicit/Console：私有 TLS 资源作用域 延迟取消到清理结束，同线程跨实例/跨 Console 重入拒绝；未交付构造实例的取消 清理、已启用异步取消的 构造函数 拒绝；Console 配置原子快照和首错保持。见 EXPLICIT_CONSOLE_SCOPE.md。

## 已移出当前交付范围的历史问题

OpenSSL 后端及构建依赖已按要求移除；不是以 suppression 隐藏 race，也没有宣称定位或
修好了系统 libcrypto。旧版两项线程报告仅适用于历史外部依赖路径。内置 SHA-256
线程测试保留；SM3 算法及其专属参数化测试已按最新需求删除，替换为原 ID/历史的拒绝回归。
Audit 功能保留，不再提供外部认证 provider 接入。当前验证见 validation/SHA256_ONLY_RESULTS.md。

## 尚未关闭

| 范围 | 边界或待办 |
|---|---|
| Global API | 本轮覆盖准入、代次、连续控制 reader、取消延迟、嵌套和最终状态。仍不支持信号处理、pthread_exit/longjmp 穿过调用、跨线程回调等待环；禁用取消期间可能等待底层 I/O，无硬实时保证。详见 GLOBAL_LIFECYCLE.md |
| Syslog | 已修非阻塞 socket/send、有限 EINTR、单调冷却重连、失败统计和实例配置。仅本地 datagram；无磁盘重试队列/已失败记录重放/远端持久确认。没有绝对 I/O 超时，文件/stdio、调度和锁竞争仍可能阻塞；真实 syslogd/旧平台矩阵待验收。见 SYSLOG_BACKEND.md |
| 文件部署边界 | 协作锁不是安全边界；不支持恶意目录替换、不协作 writer、锁 inode 删除/替换、跨版本旧 writer。每个目标的归档命名空间由同一 所有者 管理，不允许另一个 active 故意占用归档名。Production v1 明确不支持 NFS/SMB/其他网络文件系统 |
| 路径/平台收紧 | 普通文件末级 symlink/hardlink 被拒绝。内部轮换要求 RENAME_NOREPLACE，缺少能力返回 ENOTSUP，无覆盖式降级。Audit 使用 procfd 绑定路径，需要可用 procfs；Production v1 明确不支持初始化后导致 procfs 不可访问的 chroot/命名空间/procfs-unmount 场景 |
| 持久化验证 | 已增加 Release 共享库/静态库 进程 crash 恢复矩阵，以及 QEMU + raw ext4/XFS 同盘重启的 10 个 断电 点（已确认记录、Audit fsync 前后、检查点 rename 前后/commit 后、文件轮换 4 个阶段）。串口证据记录 来宾内核/文件系统。这些 CI 证据验证当前虚拟 QEMU 文件系统路径，但仍不等于物理断电、控制器/磁盘 易失缓存 或实际目标存储栈验收；输出错误后的业务重试仍可能重复 |
| API/ABI | 0.9.6 固定 66 项 公开符号；相对 0.9.5 的 62 项，在不修改旧 metrics 布局/旧函数签名的前提下新增 diagnostics 与 文件指标 共 4 个 增量式 访问接口。Production v1 平台支持范围已经定义，但稳定 ABI identity 尚未冻结；v1 前仍需完成 SONAME 1 / LOGGER_1.0 / ABI 快照 的独立迁移与 旧/新消费方 验证 |
| 平台基线 | Production v1 范围固定为 Linux/ELF x86_64 + glibc >= 2.31；32-bit、非 x86_64、musl/非 glibc 明确 不支持。Ubuntu 20.04/22.04/24.04（glibc 2.31/2.35/2.39）已有 共享库/静态库 core、installed C/C++ 消费方 与 pkg-config CI；最低 Linux 内核 仍是独立 v1 阻断项，不能由共享 运行器 内核 的 container matrix 推断 |
| 显式 / Console 取消与重入 | 本轮补齐 延迟取消 下资源入口的取消延期和同线程重入拒绝，但不是 信号安全 / 通用 AC-safe / 任意 pthread_exit、longjmp 或回调改变取消策略的保证。调用者以 ASYNCHRONOUS+ENABLE 进入普通 Logger/Console API 不受支持；直接 构造函数 明确返回 ENOTSUP。若 调用方 保留 ASYNCHRONOUS type，需先 DISABLE cancellation，库保持该 state/type。宿主仍需管理返回后的指针 清理，先停止/join 所有借用者；不得并发 destroy 或取消私有 工作线程。禁用取消期间 I/O 仍可能阻塞 |
| Audit 事务 | ATTEMPT/RESULT 不是跨业务操作原子事务，也不能跨 shutdown 用旧事务结束新生命周期。API 不是信号处理接口 |
| Audit 恢复规模 | 完整扫描保留集合、段连接 O(N²)、4096 归档上限；暂无持久 segment ID/可信恢复索引/检查点 v1 自动迁移/公开恢复告警字段 |
| 完整性信任 | 无外部可信签名、锚点或 WORM 认证；本地无密钥链不能排除整体重算、截尾或回滚。旧全零或非规范历史不自动改写 |
| Fork | raw fork 继承运行时仍拒绝。可选旧 辅助接口 仅适用于其受控单线程前置条件，不代替宿主进程管理；默认第三方集成由宿主在合适阶段创建实例 |
| 可选 fork 辅助接口 的 TSan | 历史上 TSan 残留后台 task 导致正向场景 EBUSY；本轮只复跑兼容 Release 651/651，没有宣称这些兼容场景的 TSan 全部通过 |
| 成本/性能 | 普通文件常驻 3 个 fd，完整性 Audit 约 7 个 fd；无覆盖轮换新增同步。Queue 已完成 compact/热路径 同 运行器 基准测试，证明默认 queue 内存显著下降并定位 长消息突发背压；这些结果不是整库或目标平台性能承诺。按需采集的生产者元数据 与 自节奏唤醒 已完成；当前仍待评估 消费者阶段、Syslog 批处理 等热点，见 QUEUE_STORAGE.md 与 validation/QUEUE_HOTPATH.md |
| 发布工程 | 测试与生产 archive 仍须分离；共享回归中的 --wrap 使用同源静态测试库，真实 SDK/dlclose 单独链接 .so。禁止应用手工混链私有测试库；正式安装/符号工程见 RELEASE_ENGINEERING.md；0.9.6 仍需目标平台及部署验收 |

## 发布工程候选

0.9.6 候选固定 66 项公开动态符号，其中相对 0.9.5 新增 2 个 诊断访问接口 与 2 个 文件指标 访问接口。SONAME 0、可重定位 CMake/pkg-config 下游安装契约和
XPack TGZ。没有删减功能，也没有以 package 成功替代平台/持久化验收。C++11 默认宏和本机
布局/旧头兼容有独立测试。内部符号不再动态导出，白盒测试转用同源私有静态库。
见 RELEASE_ENGINEERING.md、PLATFORM_BASELINE.md 和 validation/PACKAGING_RESULTS.md。

## 证据索引

当前：[SHA256_ONLY_RESULTS.md](../validation/SHA256_ONLY_RESULTS.md)，日志在 `validation/sha256_only/`。前轮：[NO_OPENSSL_RESULTS.md](../validation/NO_OPENSSL_RESULTS.md)，旧日志在 `validation/no_openssl/`。前轮：[EXPLICIT_CONSOLE_RESULTS.md](../validation/EXPLICIT_CONSOLE_RESULTS.md)，原日志仍在 `validation/explicit_console/`。前轮 Global：[GLOBAL_LIFECYCLE_RESULTS.md](../validation/GLOBAL_LIFECYCLE_RESULTS.md)，旧日志保留在 `validation/global_lifecycle/`。前轮 Syslog：[SYSLOG_RESULTS.md](../validation/SYSLOG_RESULTS.md)。前轮文件后端结果：[FILE_BACKEND_RESULTS.md](../validation/FILE_BACKEND_RESULTS.md)，日志仍保留。
历史 Round 1–5、controlled fork、宿主持有 的报告和原始日志均保留在 validation 中。历史普通 fork_test 的超时并非通过宣称 CPU throttling 消除；后续撤销了不成立的 child 锁重置契约。
