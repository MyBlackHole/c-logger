# Changelog

## 0.9.3 — 2026-09-25

受控发布候选验证更新。保持 public C ABI、SONAME `liblogger.so.0`、`LOGGER_0.9`
符号版本和生产运行时契约不变。新增 Release shared/static 的进程 crash 恢复矩阵，以及
QEMU + raw ext4 的 10 个 power-cut 点：已确认基线、Audit fsync 前后、checkpoint rename
前后/commit 后、文件轮换 archive rename/dirsync 与 active open/dirsync。生产构建继续
关闭 fault injection；新增 crash hook 仅存在于 test-only support target，生产宏会消除调用。

本轮验证修复了 VM 测试自身的 archive 过滤错误：active `powercut.audit.log` 不再被当作
archive 重复校验。完整 CI、release-validation、process-crash 与 VM power-cut 矩阵均通过。
这些结果仍不替代真实硬件断电、控制器/磁盘缓存、XFS、旧 kernel/glibc、32 位、NFS/SMB
和真实 syslogd 目标环境验收。

## 0.9.2 — 2026-09-25

受控发布候选更新。保持 public ABI、SONAME `liblogger.so.0` 与 `LOGGER_0.9`
符号版本。发布工程在 shared/static 两种 Release 构建上执行完整 CTest；
static 白盒回归使用同源独立测试库，避免 FORTIFY 改写 libc 调用后绕过
`--wrap` 钩子。生产静态库及运行时源码未改变。ABI 工具增加目标 ELF
架构/位数和 GLIBC 上限缺失版本的拒绝检查。目标平台与掉电验收仍待完成。

## 0.9.1 — 2026-09-25

受控生产发布候选。相对 0.9.0 保持 public ABI、SONAME `liblogger.so.0` 和
`LOGGER_0.9` 符号版本不变，集中完成 async queue 内存/热点优化、self-paced wakeup、
按需 metadata、Linux-style ownership/locking 收敛以及对应 benchmark / sanitizer 回归。

### demand-driven producer metadata

- logger create 时把 immutable `detail/include_*` 编译成 private `capture_mask`，
  producer 不再每条日志重复判断不相关 metadata。
- `MINIMAL` 不采集 module/pid/tid/context/source；`NORMAL` 只采集 module；
  `VERBOSE` 再按配置采集 pid/tid；`DEBUG` 才采集 context/source。
- PID 在实例创建时缓存；继承 runtime 的 raw-fork child 本就会在使用前 ECHILD，
  因此不再为每条需要 PID 的日志调用 `getpid()`。
- async queue 只复制实际需要的 module/context/source，未使用区域只清首字节/长度；
  固定 queue layout、bounded memory 和 source lifetime contract 不变。
- 新增 metadata capture policy regression；benchmark throughput baseline 前移到
  PR #14 合并后的 self-paced-wakeup main，避免重复计算旧优化收益。

### self-paced queue wakeup

- async producer 成功 publish 后不再每条日志都获取 `q.wait_mu` 并
  `pthread_cond_signal()`；只有观察到 `consumer_waiting=1` 才进入 slow path。
- worker 在持有 `wait_mu` 时 publish waiting 状态并重新检查 queue/running，
  保留 wait-entry 窗口的 lost-wakeup correctness。
- 多 producer 通过 atomic exchange 竞争一次 wake ownership；同一个 sleep 周期只需要
  一个 producer signal。
- shutdown/stop 改为独立 force wake，不能依赖 producer waiting hint。
- 新增 private wait/signal diagnostics 和 benchmark 输出，不改变 public metrics ABI。
- queue notify/MPSC regression 覆盖 wait-entry gap、active-backlog no-signal、
  多 producer 与 force-stop 路径。

### top-level architecture

- 新增 `docs/ARCHITECTURE.md` 作为唯一顶层架构入口，统一定义 host boundary、
  explicit/global/Console/Audit 组件关系、sync/async 数据流、线程模型、queue/backpressure、
  ownership/lifetime、locking/atomic、error/durability 和性能演进边界。
- 新增 `docs/ARCHITECTURE_INVARIANTS.md`，把 bounded memory、join-before-free、
  flush != queue-empty、no silent truncation、source lifetime isolation、sticky I/O error
  等独立成严格的 architecture review gate，避免顶层架构文档过度膨胀。
- 明确 shared MPSC、single worker、512B inline、1024 spill、BATCH_MAX=256、
  eager vsnprintf 等只是当前 implementation，可以在保持 invariant 的前提下演进。
- 修正 queue/concurrency/known-issues 中已被最新实现替代的旧描述。

### queue hot-path tuning

- 根据首次 compact benchmark 拆分热点，而不是简单扩大 spill pool。
- inline 正文上限提升到 512B，使 256B 常见日志完全绕过 spill path。
- 去除共享 `spill_mu` freelist，改为 1024-bit lock-free atomic bitmap；
  producer CAS claim，consumer atomic clear release。
- production message 直接携带 `vsnprintf()` 已知的 `text_len`，消除 async enqueue
  对长正文的第二次 O(n) 扫描。
- source snapshot 记录实际长度，只复制有效字节，减少 slot 复用时固定 512B 尾部流量。
- benchmark 改为三版本同 runner：pre-compact 做内存门禁，上一版 compact 做吞吐调优基线。

### queue benchmark validation

- 新增同 runner before/after benchmark workflow，固定 compact 前 commit
  `23504f104e900419e891b57d7187ca0bcabffdf3` 作为结构与吞吐基线。
- 默认 8192 queue 的预分配内存下降至少 50% 作为确定性 CI 门禁。
- 64/256/1024/4000B × 1/4/16 threads 各重复 3 次取 median；吞吐 ratio 只在
  baseline/candidate 都没有 drop/fallback/spill exhaustion 时计算。
- benchmark 原始样本和 Markdown 汇总作为 Actions artifact 保存，避免只保留单个
  logs/sec 数字。

### compact queue storage

- 将 MPSC queue slot 从完整 `logger_message_t` 改为 compact record：
  保留 metadata/source/context snapshot，正文使用 256B inline storage。
- 长消息使用最多 1024 个预分配 4KiB spill block；short-message hot path
  不获取 spill mutex，也不执行 per-record malloc/free。
- spill pool 用尽时不截断正文，`logger_queue_push()` 返回不可入队，
  继续走现有 per-level DROP / SYNC overflow policy。
- benchmark 新增 slot/spill/total storage 与 spill exhaustion 指标；
  regression 覆盖 inline/long roundtrip、spill block 归还以及真实 Logger
  drop/sync-fallback 行为。

### locking audit

- 新增完整 Lock Matrix，明确 global、instance、Console、Audit 的保护对象、
  固定 lock order、禁止嵌套关系和 guard 适用边界。
- 为 pthread mutex 提供 Linux-style lexical guard、checked acquire 和 try-acquire；
  checked 路径继续通过 ACQUIRE_ERR() 传播 pthread 错误。
- 首批迁移 worker/queue/Console/explicit-instance 的简单单锁临界区；
  global/Audit 多锁与 teardown 生命周期路径保持显式。
- 扩展 resource cleanup regression，验证 pthread guard 自动 unlock、
  checked acquire 与 trylock -EBUSY 语义。

### 中文代码说明规范

- 规定源码注释、ownership/locking/lifecycle 等工程说明以中文为主；API、标识符、
  标准术语、协议名和错误码保留英文，避免翻译造成技术歧义。
- 首批将 cleanup、queue、file ownership、process guard 等核心内部注释改为中文主述。
- 新增 `docs/CODING_STYLE.md`，要求后续修改到的历史英文解释性注释同步中文化。

### refcount policy

- Add REFCOUNTED as an explicit lifetime state while documenting that current
  production objects use owner/borrow, lifetime pin, join or lifecycle
  protocols instead of generic per-object reference counting.
- Add admission rules for future get/put lifetime: live-object proof on get,
  zero-is-terminal release, and checked overflow/underflow semantics.
- Mark the process `live_objects` census and global lifetime rwlock pin
  explicitly as non-refcount mechanisms.

### resource ownership audit

- Make the build dialect explicit as C11 plus GNU C extensions, matching the
  private Linux-style cleanup implementation.
- Add a repository ownership matrix covering heap objects, descriptors,
  FILE/DIR handles, workers, global lifetime pins and synchronization objects.
- Convert four single-owner rollback descriptors to `__free(close_fd)` with
  explicit `take_fd()` ownership transfer; keep error-bearing and shared
  finalization explicit.

### engineering invariants

- Add project-wide ownership, locking, concurrency, error-handling and lifecycle
  contracts around the Linux-style cleanup layer.
- Document logger field protection, worker/queue ownership, MPSC publication,
  global lock ordering and construct-before-publish/destruction rules.
- Make ownership and lifecycle design a review requirement rather than treating
  automatic cleanup as the primary resource model.


### internal resource ownership

- Adopt a Linux-style internal resource-management model: DEFINE_FREE/__free,
  no_free_ptr/return_ptr/retain_and_null_ptr, CLASS, guard/scoped_guard and
  ACQUIRE/ACQUIRE_ERR, independently implemented for user space.
- Define project rules for single ownership, declaration-at-acquisition, LIFO
  teardown, explicit ownership transfer, and no mixing of goto-unwind with
  scope-unwind in one resource graph.
- Require every managed resource to answer four ownership questions: creator,
  current owner, exact transfer point, and final releaser; distinguish OWNED,
  BORROWED, MOVED and SHARED lifetimes before choosing automatic cleanup.
- Convert async worker workspace construction and queue-slot initialization to
  automatic rollback while keeping concurrency/lifetime ownership explicit.
- Add regression coverage for free/return/retain/take ownership, class
  destructors, unconditional and conditional guards, errno preservation and
  acquisition-error reporting.


### resource footprint

- Move the async worker's batch records, formatted-line buffers and iovec
  scratch arrays from the worker stack into instance-owned heap workspace.
  Workspace capacity is capped at 256 records and shrinks with smaller queues.
- Extend benchmark JSON with queue slot/storage byte counts so the next queue
  payload redesign has an explicit memory baseline.


### production blocker hardening

- Prevent stderr EPIPE from delivering a new SIGPIPE to the host thread/process;
  preserve the caller's signal mask and process-global SIGPIPE disposition.
- Reject active file basenames in the internal `.logger.lock` / `.audit.lock`
  coordination namespaces so rotation cannot replace another owner's lock path.
- Reject unknown output bits and invalid Logger detail/rotation/file-mode/
  flush-level/overflow configuration values instead of silently accepting them.
- Add dedicated SIGPIPE, reserved-lock-namespace and invalid-config regressions.
- Remove release tests that required unsupported `ASYNC+ENABLE` entry into
  ordinary Logger/Console APIs. POSIX only guarantees three pthread cancellation
  control functions as async-cancel-safe; supported deferred and caller-disabled
  cancellation policies remain covered by contract tests.
- Add GitHub CI with a fortified shared Release profile, a complete
  unsanitized native Debug suite, and ASan/UBSan + TSan profiles. Private
  --wrap-based regression support disables FORTIFY only to keep libc hook symbol
  names stable; the production shared library remains fortified.


## 0.9.0 — release-engineering candidate

Not a final Production v1. Same complete Logger/Console/Audit functionality and
builtin SHA-256 only; no external crypto dependency or runtime implementation
changes in this revision.

- Linux ELF public symbol allowlist, hidden internals, LOGGER_0.9 symbol version
  and liblogger.so.0 SONAME; optional old fork helper remains opt-in.
- Relocatable install, CMake Logger::logger package, pkg-config, CPack TGZ and
  separate static/shared build profiles. Test support is not installed.
- Strict C++11-compatible default configuration/source macros, unchanged C
  definitions and data layout. Installed C and C++ consumers tested separately.
- A frozen prior-header consumer, layout/default checks, real dynamic symbol
  lookup and private-symbol rejection checks; PIC static embedding in SDK DSO.
- ELF/GLIBC baseline inspection gate; old target platform execution and durable
  media verification remain separate release requirements.

Compatibility: callers of undocumented internal symbols must migrate. Pre-1.0
package matching is exact; .so.0 is a candidate ABI, not a promise that all future
0.x snapshots are mutually compatible. Install static/shared variants into
separate clean prefixes. Do not exchange pointers across independent copies.
