# 变更记录

## 1.0.0 — 2026-10-06

首个稳定 Production v1。默认 shared ABI 正式冻结为 SONAME `liblogger.so.1` /
`LOGGER_1.0`，公开 C 符号集收敛为 62 项；`abi/logger-1.0.symbols` 与
`abi/LOGGER_1_0_CONTRACT.md` 分别冻结 ELF symbol surface 和语义 ABI。

pre-v1 兼容债务不进入稳定 ABI：删除无效的 `audit_config_t.fsync_each_record`、四个误导性
fork marker，以及 zero/zero/旧 prefix 配置入口。Logger/Audit/Console 配置统一采用 Linux
UAPI 风格的 size/version + future-zero-tail 规则。

资源生命周期按 quiescent-before-free 收口：worker join、同步对象 teardown、process census
成为 final release 的生命周期证明；无法证明 quiescent 时禁止继续 free。Audit recovery/
checkpoint 改为 owned `dirfd + basename`，instance ID entropy 改为 nonblocking fail-fast；
4096 archive segment 连接由 O(N²) 优化为 O(N log N)。

Production v1 支持范围固定为 Linux/ELF/x86_64、glibc >= 2.31、Linux >= 5.10、本地 ext4/XFS
以及本地 Unix-domain datagram Syslog。最低内核、real syslogd、glibc runtime、crash/power-cut、
Sanitizer、4096 archive capacity、installed consumer 与 release packaging 门禁均已建立并通过。

从 0.9.x 升级必须使用 1.0.0 headers 重新构建；SONAME 0 不承诺与稳定 v1 二进制兼容。
详细迁移与不支持范围见 `docs/RELEASE_NOTES_1.0.0.md`。

## 0.9.6 — 2026-10-04

受控生产发布候选的可观测性收敛版本。保持 ABI major `0`、SONAME
`liblogger.so.0`、`LOGGER_0.9` 符号版本 以及既有 public 函数签名和旧 metrics
结构布局不变；公开 C 符号集 从 0.9.5 的 62 项 增量式 增至 66 项。

新增统一 运行时诊断：显式实例与 全局门面 均可读取 队列深度/容量、
高水位、完成积压、队列存储、溢出区使用/耗尽、工作线程等待/唤醒、
丢弃/同步回退、输出记账、粘滞首个错误 及 lifetime observation flags。
统一 diagnostics 不执行 后端 I/O、flush 或 reconnect，供宿主采样并接入现有监控系统。

新增 文件后端指标：区分顶层 写操作 与 write/writev 系统调用，保留 terminal
error 前 部分字节，分别统计 EINTR 重试、data/dir fsync、轮转、重新打开、保留，
并暴露 最近一次顶层结果 与 sticky write/sync/轮转/重新打开 errno。快照只在读取一致
File sink 状态时取得现有 `emit_mu`，不会触发 I/O 或状态修复。

发布文档同时统一 ext4 + XFS QEMU/原始磁盘 断电 边界：两种文件系统均执行同一组 10
个 切断点；该证据仍不替代真实服务器、电源、控制器/设备易失缓存、实际目标
挂载/存储栈 或最低 内核/glibc 验收。

项目分发许可正式明确为 Apache License 2.0；根目录与 XPack/安装树 均携带权威
`LICENSE`，打包门禁 对许可文件存在性和版本文本做 失败关闭 验证。

## 0.9.5 — 2026-09-28

受控生产发布候选的平台验证与发布恢复收敛版本。相对 0.9.4 **没有 `src/` / `include/`
运行时代码变化**，继续保持 public C API/ABI、SONAME `liblogger.so.0`、
`LOGGER_0.9` 符号版本 和默认 62 项 公开符号集 不变。

本版把 QEMU/原始磁盘 断电 从 ext4 扩展到 ext4 + XFS，两种文件系统都执行同一组
10 个 切断点、宿主 SIGKILL、同盘重启、恢复、追加和 Audit chain 验证；串口证据同时
记录 来宾内核 与 已挂载文件系统。XFS 为模块时，验证 harness 将所选 来宾内核
对应的 XFS 模块/依赖 打入最小 initramfs。

新增 x86_64 glibc 运行时矩阵，在 Ubuntu 20.04 / 22.04 / 24.04
（glibc 2.31 / 2.35 / 2.39）分别验证 共享库/静态库 生产库链接 core、
installed C/C++ 消费方 与 pkg-config 消费方。shared ELF 同时以容器实际 glibc 作为
`--max-glibc` 上限；这建立的是已验证 用户态环境 范围，不代表最低 Linux 内核。

发布链路改为可恢复幂等：已有 发布 只有在 pre发布 元数据与四个远端资产全部正确且
SHA-256 验证通过时才跳过；不完整 发布 会锁定既有 tag 对应源码重新构建，并使用
`gh release upload --clobber` 修复同名资产，发布后再次下载远端资产验证。

兼容性工具补齐旧 用户态环境 差异：root-owned container 的 Xmake wrapper、旧 binutils 的
symbol-version 输出处理，以及老 pkg-config 对带空格 relocation 路径的工具限制隔离。
真实物理断电、控制器/设备易失缓存、最低 内核、32-bit、NFS/SMB 与真实
syslogd 仍属于后续目标环境验收范围。

## 0.9.4 — 2026-09-27

受控生产发布候选的构建/发布权威收敛版本。保持 public C API/ABI、SONAME
`liblogger.so.0`、`LOGGER_0.9` 符号版本 和默认 62 项 公开符号集 不变；
运行时行为不因构建系统迁移而改变。

本版完成 Xmake 单一权威迁移：全部 production/test/regression/Sanitizer、focused crash、
QEMU 断电、当前 queue 基准测试 candidate、install、XPack 与 GitHub Release 都由
Xmake 驱动；根 CMake build/CTest/CPack 与旧 打包 辅助接口 删除。下游 CMake 兼容仍保留，
发布包继续生成 `LoggerConfig.cmake` 并用真实独立 消费方 验证
`find_package(Logger ... CONFIG)`。

ABI allowlist 从 `cmake/logger.symbols` 迁到中立的 `abi/logger.symbols`。package gate
直接读取 `VERSION`，不再硬编码候选版本；本地 `scripts/check.sh` 和辅助 基准测试/
cross-version 工具也适配 Xmake 输出布局。

## 0.9.3 — 2026-09-25

受控发布候选验证更新。保持 public C ABI、SONAME `liblogger.so.0`、`LOGGER_0.9`
符号版本和生产运行时契约不变。新增 Release 共享库/静态库 的进程 crash 恢复矩阵，以及
QEMU + raw ext4 的 10 个 断电 点：已确认基线、Audit fsync 前后、检查点 rename
前后/commit 后、文件轮换 archive rename/dirsync 与 active open/dirsync。生产构建继续
关闭 故障注入；新增 crash hook 仅存在于 test-only support target，生产宏会消除调用。

本轮验证修复了 VM 测试自身的 archive 过滤错误：active `powercut.audit.log` 不再被当作
archive 重复校验。完整 CI、发布-validation、进程崩溃 与 VM 断电 矩阵均通过。
这些结果仍不替代真实硬件断电、控制器/磁盘缓存、XFS、旧 内核/glibc、32 位、NFS/SMB
和真实 syslogd 目标环境验收。

## 0.9.2 — 2026-09-25

受控发布候选更新。保持 public ABI、SONAME `liblogger.so.0` 与 `LOGGER_0.9`
符号版本。发布工程在 共享库/静态库 两种 Release 构建上执行完整 CTest；
static 白盒回归使用同源独立测试库，避免 FORTIFY 改写 libc 调用后绕过
`--wrap` 钩子。生产静态库及运行时源码未改变。ABI 工具增加目标 ELF
架构/位数和 GLIBC 上限缺失版本的拒绝检查。目标平台与掉电验收仍待完成。

## 0.9.1 — 2026-09-25

受控生产发布候选。相对 0.9.0 保持 public ABI、SONAME `liblogger.so.0` 和
`LOGGER_0.9` 符号版本不变，集中完成 async queue 内存/热点优化、自节奏唤醒、
按需 元数据、Linux-style 所有者ship/locking 收敛以及对应 基准测试 / Sanitizer 回归。

### 按需采集的生产者元数据

- logger create 时把 immutable `detail/include_*` 编译成 private `capture_mask`，
  producer 不再每条日志重复判断不相关 元数据。
- `MINIMAL` 不采集 module/pid/tid/context/源信息；`NORMAL` 只采集 module；
  `VERBOSE` 再按配置采集 pid/tid；`DEBUG` 才采集 context/源信息。
- PID 在实例创建时缓存；继承 runtime 的 raw-fork child 本就会在使用前 ECHILD，
  因此不再为每条需要 PID 的日志调用 `getpid()`。
- async queue 只复制实际需要的 module/context/源信息，未使用区域只清首字节/长度；
  固定 queue layout、bounded memory 和 源信息 lifetime 契约 不变。
- 新增 元数据 capture policy regression；基准测试 throughput baseline 前移到
  PR #14 合并后的 self-paced-wakeup main，避免重复计算旧优化收益。

### self-paced queue wakeup

- async producer 成功 publish 后不再每条日志都获取 `q.wait_mu` 并
  `pthread_cond_signal()`；只有观察到 `consumer_waiting=1` 才进入 slow path。
- 工作线程 在持有 `wait_mu` 时 publish waiting 状态并重新检查 queue/running，
  保留 wait-entry 窗口的 lost-wakeup correctness。
- 多 producer 通过 atomic exchange 竞争一次 wake 所有者ship；同一个 sleep 周期只需要
  一个 producer signal。
- shutdown/stop 改为独立 force wake，不能依赖 producer waiting hint。
- 新增 private wait/signal diagnostics 和 基准测试 输出，不改变 public metrics ABI。
- queue notify/MPSC regression 覆盖 wait-entry gap、active-backlog no-signal、
  多 producer 与 force-stop 路径。

### top-level 架构

- 新增 `docs/ARCHITECTURE.md` 作为唯一顶层架构入口，统一定义 宿主 boundary、
  explicit/global/Console/Audit 组件关系、sync/async 数据流、线程模型、queue/backpressure、
  所有者ship/lifetime、locking/atomic、error/持久性 和性能演进边界。
- 新增 `docs/ARCHITECTURE_INVARIANTS.md`，把 bounded memory、join-before-free、
  flush != queue-empty、no silent truncation、源信息 lifetime isolation、粘滞 I/O 错误 error
  等独立成严格的 架构评审 gate，避免顶层架构文档过度膨胀。
- 明确 shared MPSC、single 工作线程、512B inline、1024 spill、BATCH_MAX=256、
  eager vsnprintf 等只是当前 implementation，可以在保持 invariant 的前提下演进。
- 修正 queue/concurrency/known-issues 中已被最新实现替代的旧描述。

### queue 热路径 tuning

- 根据首次 compact 基准测试 拆分热点，而不是简单扩大 spill pool。
- inline 正文上限提升到 512B，使 256B 常见日志完全绕过 spill path。
- 去除共享 `spill_mu` freelist，改为 1024-bit lock-free atomic bitmap；
  producer CAS claim，消费方 atomic clear 发布。
- production message 直接携带 `vsnprintf()` 已知的 `text_len`，消除 async enqueue
  对长正文的第二次 O(n) 扫描。
- 源信息 快照 记录实际长度，只复制有效字节，减少 slot 复用时固定 512B 尾部流量。
- 基准测试 改为三版本同 运行器：pre-compact 做内存门禁，上一版 compact 做吞吐调优基线。

### queue 基准测试 validation

- 新增同 运行器 before/after 基准测试 workflow，固定 compact 前 commit
  `23504f104e900419e891b57d7187ca0bcabffdf3` 作为结构与吞吐基线。
- 默认 8192 queue 的预分配内存下降至少 50% 作为确定性 CI 门禁。
- 64/256/1024/4000B × 1/4/16 threads 各重复 3 次取 median；吞吐 ratio 只在
  baseline/candidate 都没有 drop/fallback/spill exhaustion 时计算。
- 基准测试 原始样本和 Markdown 汇总作为 Actions artifact 保存，避免只保留单个
  logs/sec 数字。

### compact 队列存储

- 将 MPSC queue slot 从完整 `logger_message_t` 改为 compact record：
  保留 元数据/源信息/context 快照，正文使用 256B inline storage。
- 长消息使用最多 1024 个预分配 4KiB spill block；short-message hot path
  不获取 spill mutex，也不执行 per-record malloc/free。
- spill pool 用尽时不截断正文，`logger_queue_push()` 返回不可入队，
  继续走现有 per-level DROP / SYNC overflow policy。
- 基准测试 新增 slot/spill/total storage 与 spill exhaustion 指标；
  regression 覆盖 inline/long roundtrip、spill block 归还以及真实 Logger
  丢弃/同步回退 行为。

### locking audit

- 新增完整 Lock Matrix，明确 global、instance、Console、Audit 的保护对象、
  固定 lock order、禁止嵌套关系和 guard 适用边界。
- 为 pthread mutex 提供 Linux-style lexical guard、checked acquire 和 try-acquire；
  checked 路径继续通过 ACQUIRE_ERR() 传播 pthread 错误。
- 首批迁移 工作线程/queue/Console/explicit-instance 的简单单锁临界区；
  global/Audit 多锁与 teardown 生命周期路径保持显式。
- 扩展 资源 清理 regression，验证 pthread guard 自动 unlock、
  checked acquire 与 trylock -EBUSY 语义。

### 中文代码说明规范

- 规定源码注释、所有者ship/locking/lifecycle 等工程说明以中文为主；API、标识符、
  标准术语、协议名和错误码保留英文，避免翻译造成技术歧义。
- 首批将 清理、queue、file 所有者ship、process guard 等核心内部注释改为中文主述。
- 新增 `docs/CODING_STYLE.md`，要求后续修改到的历史英文解释性注释同步中文化。

### refcount policy

- Add REFCOUNTED as an explicit lifetime state while documenting that current
  production objects use 所有者/borrow, lifetime pin, join or lifecycle
  protocols instead of generic per-object reference counting.
- Add admission rules for future get/put lifetime: live-object proof on get,
  zero-is-terminal 发布, and checked overflow/underflow semantics.
- Mark the process `live_objects` census and global lifetime rwlock pin
  explicitly as non-refcount mechanisms.

### 资源 所有者ship audit

- Make the build dialect explicit as C11 plus GNU C 扩展, matching the
  private Linux-style 清理 implementation.
- Add a repository 所有者ship matrix covering heap objects, descriptors,
  FILE/DIR handles, 工作线程s, global lifetime pins and synchronization objects.
- Convert four single-所有者 rollback descriptors to `__free(close_fd)` with
  explicit `take_fd()` 所有者ship transfer; keep error-bearing and shared
  finalization explicit.

### engineering invariants

- Add project-wide 所有者ship, locking, concurrency, error-handling and lifecycle
  契约s around the Linux-style 清理 layer.
- Document logger field protection, 工作线程/queue 所有者ship, MPSC publication,
  global lock ordering and construct-before-publish/destruction rules.
- Make 所有者ship and lifecycle design a review requirement rather than treating
  automatic 清理 as the primary 资源 model.


### internal 资源 所有者ship

- Adopt a Linux-style internal 资源-management model: DEFINE_FREE/__free,
  no_free_ptr/return_ptr/retain_and_null_ptr, CLASS, guard/scoped_guard and
  ACQUIRE/ACQUIRE_ERR, independently implemented for user space.
- Define project rules for single 所有者ship, declaration-at-acquisition, LIFO
  teardown, explicit 所有者ship transfer, and no mixing of goto-unwind with
  scope-unwind in one 资源 graph.
- Require every managed 资源 to answer four 所有者ship questions: creator,
  current 所有者, exact transfer point, and final 发布r; distinguish OWNED,
  BORROWED, MOVED and SHARED lifetimes before choosing automatic 清理.
- Convert async 工作线程 workspace construction and queue-slot initialization to
  automatic rollback while keeping concurrency/lifetime 所有者ship explicit.
- Add regression coverage for free/return/retain/take 所有者ship, class
  destructors, unconditional and conditional guards, errno p预留 and
  acquisition-error reporting.


### 资源 footprint

- Move the async 工作线程's batch records, formatted-line buffers and iovec
  scratch arrays from the 工作线程 stack into instance-owned heap workspace.
  Workspace capacity is capped at 256 records and shrinks with smaller queues.
- Extend 基准测试 JSON with queue slot/storage byte counts so the next queue
  payload redesign has an explicit memory baseline.


### production 阻断项 hardening

- Prevent 标准错误 EPIPE from delivering a new SIGPIPE to the 宿主 thread/process;
  preserve the 调用方's signal mask and process-global SIGPIPE disposition.
- Reject active file basenames in the internal `.logger.lock` / `.audit.lock`
  coordination 命名空间s so 轮转 cannot replace another 所有者's lock path.
- Reject unknown output bits and invalid Logger detail/轮转/file-mode/
  flush-level/overflow configuration values instead of silently accepting them.
- Add dedicated SIGPIPE, reserved-lock-命名空间 and invalid-config regressions.
- Remove 发布 tests that required 不支持 `ASYNC+ENABLE` entry into
  ordinary Logger/Console APIs. POSIX only guarantees three pthread cancellation
  control functions as async-cancel-safe; supported deferred and 调用方-disabled
  cancellation policies remain covered by 契约 tests.
- Add GitHub CI with a fortified shared Release profile, a complete
  unsanitized native Debug suite, and ASan/UBSan + TSan profiles. Private
  --wrap-based 回归支持 disables FORTIFY only to keep libc hook symbol
  names stable; the production shared library remains fortified.


## 0.9.0 — 发布-engineering candidate

Not a final Production v1. Same complete Logger/Console/Audit functionality and
builtin SHA-256 only; no external crypto dependency or runtime implementation
changes in this revision.

- Linux ELF 公开符号允许列表, hidden internals, LOGGER_0.9 符号版本
  and liblogger.so.0 SONAME; optional old fork 辅助接口 remains opt-in.
- Relocatable install, CMake Logger::logger package, pkg-config, CPack TGZ and
  separate static/共享库构建 profiles. Test support is not installed.
- Strict C++11-compatible default configuration/源信息 macros, unchanged C
  definitions and data layout. Installed C and C++ 消费方s tested separately.
- A frozen prior-header 消费方, layout/default checks, real dynamic symbol
  lookup and private-symbol rejection checks; PIC static embedding in SDK DSO.
- ELF/GLIBC baseline inspection gate; old target platform execution and durable
  media verification remain separate 发布 requirements.

Compatibility: 调用方s of undocumented internal symbols must migrate. Pre-1.0
package matching is exact; .so.0 is a candidate ABI, not a promise that all future
0.x 快照s are mutually compatible. Install static/shared variants into
separate clean prefixes. Do not exchange pointers across independent copies.
