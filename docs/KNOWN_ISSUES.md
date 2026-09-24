# 尚未关闭的发布阻断项（仅内置 SHA-256）

本项目仍为缺陷收敛开发版，不是 Production v1。以本表和当前验证报告为准；旧轮次报告保留历史证据，不表示旧用法仍受支持。

## 已完成的定向修复

- Queue/Flush：等待通知协议、完成水位、sticky I/O 与状态 flush。
- Audit 生命周期：实例资源归属、START/STOP 准入、checkpoint 失败暂停和 I/O 不确定结果停写。
- Crypto：可返回错误的摘要 provider、CRYPTO_FAILED、历史 SM3 移位 UB 已修；当前 SM3 已按需求删除，不背书旧可疑历史。
- 解析/恢复：严格 KV 和有界行读取、EOF 残片证据、按已验证 hash 边连接保留集合。
- Fork/测试隔离：共享继承状态防护，默认生产库没有故障/退出钩子与扫描/创建进程 helper。
- Host-owned：SDK 日志回调、实例借用、异步 source 有界快照、strict dispose、实际 DSO 集成。
- 文件后端：受信任本地目录下单目标 owner、固定 dirfd、无覆盖时间轮换、reopen 保留旧 fd、资源错误清理；Audit 在恢复之前取得 active/state 所有权。
- Syslog：独立配置有界复制、非阻塞输出、显式背压、仅后续记录触发的有界重连、sink 指标和旧 version-1 config 前缀读取。
- Global：统一 ticket（generation + phase）准入，读侧/延迟 shutdown 复核，控制 reader 关闭门禁；取消延迟到释放 pin/锁之后，同线程嵌套拒绝；新增状态 shutdown。没有改变宿主拥有显式实例的推荐模型。

- Explicit/Console：私有 TLS resource scope 延迟取消到清理结束，同线程跨实例/跨 Console 重入拒绝；未交付构造实例的取消 cleanup、已启用异步取消的 constructor 拒绝；Console 配置原子快照和首错保持。见 EXPLICIT_CONSOLE_SCOPE.md。

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
| 文件部署边界 | 协作锁不是安全边界；不支持恶意目录替换、不协作 writer、锁 inode 删除/替换、跨版本旧 writer。每个目标的归档命名空间由同一 owner 管理，不允许另一个 active 故意占用归档名。NFS/SMB 未验收 |
| 路径/平台收紧 | 普通文件末级 symlink/hardlink 被拒绝。内部轮换要求 RENAME_NOREPLACE，缺少能力返回 ENOTSUP，无覆盖式降级。Audit 使用 procfd 绑定路径，需要可用 procfs；未验收初始化后 chroot/卸载 procfs |
| 持久化验证 | 本轮环境 OverlayFS `fsync=volatile`，仅验收过程逻辑和进程退出，不等于系统掉电。需非 volatile ext4/XFS 与实际存储/VM 故障矩阵；输出错误后的业务重试仍可能重复 |
| API/ABI | 公共签名和布局本轮未改。本轮 Logger config 的旧 v1 前缀/完整新尾部已有 guard-page 和独立旧头 consumer 验证；不表示所有结构、架构或前向兼容已冻结，时区变更等仍需验证；已完成 0.9.0 候选的显式导出/安装/SONAME 与本机旧头验证；v1 ABI 和跨平台矩阵尚未正式冻结 |
| 老平台 | Linux3.x、老 glibc、32bit 未完整验收。外部密码库后端现已移除；不再承诺外部密码模块集成。fork guard 依赖 lock-free int；syscall 兼容写法不代表所有旧文件系统支持内部轮换 |
| 显式 / Console 取消与重入 | 本轮补齐 deferred cancellation 下资源入口的取消延期和同线程重入拒绝，但不是 signal-safe / 通用 AC-safe / 任意 pthread_exit、longjmp 或回调改变取消策略的保证。调用者以 ASYNCHRONOUS+ENABLE 进入普通 Logger/Console API 不受支持；直接 constructor 明确返回 ENOTSUP。若 caller 保留 ASYNCHRONOUS type，需先 DISABLE cancellation，库保持该 state/type。宿主仍需管理返回后的指针 cleanup，先停止/join 所有借用者；不得并发 destroy 或取消私有 worker。禁用取消期间 I/O 仍可能阻塞 |
| Audit 事务 | ATTEMPT/RESULT 不是跨业务操作原子事务，也不能跨 shutdown 用旧事务结束新生命周期。API 不是信号处理接口 |
| Audit 恢复规模 | 完整扫描保留集合、段连接 O(N²)、4096 归档上限；暂无持久 segment ID/可信恢复索引/checkpoint v1 自动迁移/公开恢复告警字段 |
| 完整性信任 | 无外部可信签名、锚点或 WORM 认证；本地无密钥链不能排除整体重算、截尾或回滚。旧全零或非规范历史不自动改写 |
| Fork | raw fork 继承运行时仍拒绝。可选旧 helper 仅适用于其受控单线程前置条件，不代替宿主进程管理；默认第三方集成由宿主在合适阶段创建实例 |
| 可选 fork helper 的 TSan | 历史上 TSan 残留后台 task 导致正向场景 EBUSY；本轮只复跑兼容 Release 651/651，没有宣称这些兼容场景的 TSan 全部通过 |
| 成本/性能 | 普通文件常驻 3 个 fd，完整性 Audit 约 7 个 fd；无覆盖轮换新增同步。本轮资源入口新增 pthread 取消状态保存/恢复和 TLS 检查；没有本轮吞吐数据，历史微基准不作为当前性能承诺 |
| 发布工程 | 测试与生产 archive 仍须分离；共享回归中的 --wrap 使用同源静态测试库，真实 SDK/dlclose 单独链接 .so。禁止应用手工混链私有测试库；正式安装/符号工程见 RELEASE_ENGINEERING.md；0.9.0 仍需目标平台及部署验收 |

## 发布工程候选

0.9.0 已建立 62 项公开动态符号、SONAME 0、可重定位 CMake/pkg-config 安装、CPack TGZ。
没有删减功能，也没有以 package 成功替代平台/持久化验收。C++11 默认宏和本机布局/旧头
兼容有独立测试。内部符号不再动态导出，白盒测试转用同源私有静态库。
见 RELEASE_ENGINEERING.md、PLATFORM_BASELINE.md 和 validation/PACKAGING_RESULTS.md。

## 证据索引

当前：[SHA256_ONLY_RESULTS.md](../validation/SHA256_ONLY_RESULTS.md)，日志在 `validation/sha256_only/`。前轮：[NO_OPENSSL_RESULTS.md](../validation/NO_OPENSSL_RESULTS.md)，旧日志在 `validation/no_openssl/`。前轮：[EXPLICIT_CONSOLE_RESULTS.md](../validation/EXPLICIT_CONSOLE_RESULTS.md)，原日志仍在 `validation/explicit_console/`。前轮 Global：[GLOBAL_LIFECYCLE_RESULTS.md](../validation/GLOBAL_LIFECYCLE_RESULTS.md)，旧日志保留在 `validation/global_lifecycle/`。前轮 Syslog：[SYSLOG_RESULTS.md](../validation/SYSLOG_RESULTS.md)。前轮文件后端结果：[FILE_BACKEND_RESULTS.md](../validation/FILE_BACKEND_RESULTS.md)，日志仍保留。
历史 Round 1–5、controlled fork、host-owned 的报告和原始日志均保留在 validation 中。历史普通 fork_test 的超时并非通过宣称 CPU throttling 消除；后续撤销了不成立的 child 锁重置契约。
