> 新增受控 `logger_fork_reinit()`：允许初始化之后 fork，父子无需 exec 即可重新初始化。
> 前提是先停止业务线程、关闭 Audit 与显式实例；辅助接口在父进程清理默认 Logger。
> 这不是任意多线程原始 fork 自动恢复。详见 `docs/CONTROLLED_FORK_REINIT.md`。

> **当前交付：第 5 轮 Fork/生产隔离修复，仍非生产版 v1。**
> 子进程不能靠覆盖 pthread 对象重建运行时；继承状态必须 exec 后再初始化。
> 生产 `logger` 与 `logger_test_support` 分开构建，不由环境变量开启生产故障注入。
> 当前契约见 [ROUND5_FORK_ISOLATION](ROUND5_FORK_ISOLATION.md)，实际验证及未通过项见 [ROUND5_RESULTS](../validation/ROUND5_RESULTS.md)。
> 队列/刷新、Audit 状态、密码与解析分别见第 1–4 轮文档；剩余问题见 [KNOWN_ISSUES](KNOWN_ISSUES.md)。
> 下方按迭代记录保留的测试/性能数字与设计叙述属于历史，不能替代最新契约或发布门禁。

# 生产级 C Logger

面向 Linux/POSIX 的 C11 Logger。

主要功能：
- 私有的进程全局默认 Logger，以及显式 `logger_t` 实例；
- 同步日志或有界异步日志；
- TRACE/DEBUG/INFO/WARN/ERROR/FATAL 级别；
- stderr、轮转文件和 Syslog 输出；
- PID/TID/模块/源元数据；
- 运行时日志级别修改；
- 队列溢出计数；
- 异步队列满时，ERROR/FATAL 同步回退到后端；
- 关闭时平滑排空队列；
- 父进程初始化前可回退到 stderr；关闭后关闭新请求准入。

构建：

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

默认全局门面有意把 `g_logger` 保持为 `logger_global.c` 的私有状态。
对于审计/安全日志，应创建独立的显式 Logger，而不是把 Audit 记录混入普通运行日志。

## 高并发修订

全局门面现在只使用一个进程级 `pthread_rwlock_t` 来固定 Logger 生命周期。
普通生产者获取共享/读锁，因此生产者线程不再被旧的全局 mutex 串行化。
关闭获取写锁、摘除全局指针，然后安全地排空并销毁 Logger。

队列仍然有界。队列满时，TRACE/DEBUG/INFO/WARN 有意采用非阻塞策略；
ERROR/FATAL 会同步回退到后端。项目同时提供基准目标 `bench_logger`。

这是偏保守的生产优化：消除全局串行化瓶颈，同时保持直接、容易验证的关闭正确性。
真正的无锁 MPSC 队列或 epoch reclamation 设计当然可以实现，但只有在性能分析证明
rwlock 或队列 mutex 已经成为下一处瓶颈后才值得引入。

## MPSC 与批处理修订

异步队列现在是有界、2 的幂容量的 MPSC 序列环形队列。生产者不再获取旧队列 mutex。
单消费者每次最多排空 64 条记录，并使用 `writev()` 对文件/stderr 进行批量写入。

条件变量只负责空队列时的休眠/唤醒，不承担队列所有权保护。关闭设置 `running=0`，
唤醒消费者；消费者退出前会排空所有已经发布的记录。

Logger 实现现在使用 `-Werror` 构建。

## 背压与可观测性修订

队列溢出策略可以按严重级别配置。默认 TRACE/DEBUG/INFO/WARN 使用 DROP，
ERROR/FATAL 使用同步回退。指标暴露逐级别丢弃数、队列高水位、成功入队数、
同步回退次数、消费者批次数和消费者记录数。

这些计数器使用 relaxed atomic，定位是遥测，而不是事务记账。
并发情况下的队列深度/高水位按设计就是近似值。

## 时间戳轮转修订

已经移除 `.1/.2/...` 数字归档。活动文件保持稳定名称，例如 `xxx.log` 和
`xxx.audit.log`。归档文件使用 UTC 时间戳：

```text
xxx.20260922T144531.123456Z.log
xxx.20260922T144531.123456Z.audit.log
```

轮转模式包括 NONE、SIZE、DAILY、SIZE_DAILY 和外部。保留周期以天表示。
清理逻辑只删除严格匹配当前 Logger 时间戳归档命名规则的文件。
`logger_reopen[_instance]()` 支持类似 logrotate 的外部所有者，不需要在信号处理器中执行不安全操作。

## Audit 子系统

`audit.h` / `audit.c` 增加了独立的结构化 Audit 层，其后端使用独立 Logger 实例。
名为 `xxx` 的服务把运行日志写入 `xxx.log`，把审计记录写入 `xxx.audit.log`。

Audit 使用稳定的单行 key/value 文本，而不是 JSON。字符串值带引号，并转义反斜杠、引号、
换行、回车和制表符。Audit 路径由 `log_dir + name + ".audit.log"` 派生。

第一版有意使用同步 Audit 写入，并且可以对每条记录执行 fsync，从而避免静默队列丢弃。
历史限制是当时 Logger 后端写 API 返回 void，精确的 write/fsync 失败还不能通过 `audit_write()`
向上传播。因此后续可靠性工作是在把 AUDIT_FAIL_DENY 用作严格安全操作门禁之前，先增加返回状态的后端路径。

## 严格 Audit I/O 状态修订

新增返回状态的同步后端 API：

```text
logger_log_sync_status(...)
```

它绕过异步队列，同步写入记录并强制执行 `fsync()`，返回 `0` 或负 errno。
`audit_write()` 现在使用该路径，并把后端失败转换为 `-1`，同时设置 `errno`。

这样，ENOSPC、EBADF、EIO 和 fsync 错误等本地文件 Audit 故障就能被业务逻辑观察到，
调用方在配置 AUDIT_FAIL_DENY 时可以实现失败关闭的安全操作。

需要注意：libc `syslog()` 不提供端到端投递确认，因此严格 Audit 持久性依赖专用本地审计文件，
不能只依赖 Syslog 后端。

## Audit 事务语义

安全敏感操作现在可以使用共享 transaction id 的 ATTEMPT/RESULT 对：

```text
audit_begin(&事件);   // durable ATTEMPT before 操作
... protected 操作 ...
audit_end(&事件, AUDIT_SUCCESS, 0);
```

使用 AUDIT_FAIL_DENY 时，如果 `audit_begin()` 失败，调用方不应执行受保护操作。
结果记录描述真实执行结果。崩溃后只有 ATTEMPT 而没有结果的情况会被有意保留下来，
Audit 审阅/恢复时可以把它识别为未完成操作。

`seq` 用于排列当前进程生命周期中的 Audit 记录；`txn` 用于关联 ATTEMPT 与结果，
二者用途不同。

## CLI Console 前端

`console.h` 有意与运行日志和 Audit 日志分离。

- `console_print()` 是稳定的 stdout 结果/数据通道，不添加标签或 ANSI 序列；
- `console_info/warn/error()` 是写往 stderr 的人工诊断信息；
- `console_verbose/debug()` 由 verbosity 控制；
- 颜色支持 AUTO/ALWAYS/NEVER，AUTO 使用 `isatty(stderr)`；
- Console 写入进行串行化，避免 CLI 工作线程的消息在 stdio 调用粒度发生交错。

推荐映射：
`--quiet` -> CONSOLE_QUIET，默认 -> NORMAL，`-v` -> VERBOSE，
`--debug` -> DEBUG，`--no-color` -> COLOR_NEVER。

该修订还修正了 Audit ATTEMPT 语义：ATTEMPT 记录不再错误声明 `result=SUCCESS`；
result/error 字段只在 RESULT 记录中输出。

## 统一运行时记录与源码位置修订

运行日志行现在统一为：
`timestamp level pid tid [module] file:line function() message`。

时间戳使用本地墙钟时间、6 位小数，并显式输出数字时区偏移，例如：
`2026-09-22T22:35:41.123456+0800`。

新增 `logger_source_t` / `LOGGER_SOURCE()` 作为源码位置信息模型，为
module/file/function/line 元数据提供稳定承载位置，避免未来公开 API 持续增加位置参数。
现有 `LOG_*` 宏保持源码兼容。

Console 输出有意保持不含运行时元数据。Audit 输出保留 pid/tid，但不加入源 file/line/function，
因为这些属于实现细节，而不是需要长期持久化的 Audit 语义。

## 详细程度策略与请求作用域上下文

运行时 Logger detail 现在与 severity 解耦：
MINIMAL = time/level/message；NORMAL 增加 module；VERBOSE 增加 pid/tid；
DEBUG 增加 request/session/trace context 以及源 file/line/function。

请求上下文使用线程本地存储，并在异步入队前复制到每条记录中，因此工作线程格式化不会引用
调用方短生命周期内存。在请求入口调用 `logger_context_set()`，退出时调用 `logger_context_clear()`。

Console 调试新增能够感知源的 `CONSOLE_DEBUG(...)` 宏。普通 Console 输出保持干净。
Audit schema 固定，不受 Logger detail 设置影响。

## 生产加固评审修订

这一轮重点是正确性，而不是新增表面功能。

- 批量 `writev()` 现在正确处理 EINTR 和部分写；
- 文件 mode 可配置，运行日志默认 0640，Audit 文件默认 0600；
- 时间戳轮转在 re名称/重新打开后对父目录执行 fsync，以增强崩溃后的元数据持久性；
- 已有关闭生命周期锁、有界 MPSC 行为、严格 Audit write/fsync 状态、时间戳归档匹配和 TLS
  上下文复制，都重新通过测试套件验证。

明确记录的剩余设计限制包括：初始化后 fork 如果没有显式 atfork/reinit 契约则不受支持；
libc Syslog 具有进程全局状态且没有持久投递确认；Audit sequence 和 transaction id 仍然只是
进程生命周期内的值，而不是跨重启持久标识。

## Audit 进程实例身份

每次 `audit_init()` 成功都会使用 Linux `getrandom()` 生成密码学强度的 128 位实例 id，
并编码为 32 个小写十六进制字符。每条 Audit 记录都包含 `instance=<id>`。
`seq` 和 `txn` 仍然是低成本的进程实例计数器，因此稳定身份分别是
`(instance,seq)` 和 `(instance,txn)`。

Audit 初始化会写入持久化 `AUDIT_START` 记录，正常关闭会写入 `AUDIT_STOP`。
如果某个新 START 之前没有看到前一实例对应的 STOP，这可以作为异常终止的重要证据，
但单凭这一点不能证明具体原因。

实例 id 有意不持久化，也不从 PID/时间派生；每次重启都会创建新身份。

## Audit 完整性链

Audit 记录默认使用 SHA-256 哈希链。哈希覆盖从 `instance=` 开始、直到 `prev=` 字段为止的
规范化 Audit 载荷；普通 Logger 的 timestamp/PID/TID 前缀有意排除在完整性域之外。

每条记录都携带 `prev=<64 hex>` 与 `hash=<64 hex>`。某个 Audit 实例的第一条记录使用全零前序哈希。
`audit_verify_file(path)` 可以检测单个活动/归档文件内部的记录篡改和链断裂。

这只是篡改**检测**，不是篡改阻止：如果攻击者能够重写整个文件，也可以重新计算无密钥哈希链。
外部签名/WORM 检查点属于后续加固层。

该阶段还没有持久化/恢复独立文件之间的轮转连续性，这是有意留给下一步解决的问题，
不能假装单文件验证器已经证明跨文件连续性。

## Audit 链跨重启/轮转连续性

Audit 链头现在持久化到小型 0600 状态文件：`<name>.audit.state`，或由 `chain_state_path` 指定。
每条 Audit 记录成功持久化后，会通过“临时文件 + fsync + re名称 + 目录 fsync”原子更新状态文件。
后续进程实例加载该 hash 作为自己的第一个 `prev`。

这样可以让哈希链跨正常重启和时间戳轮转继续延伸。
`audit_verify_file_from()` 从预期前序 hash 开始验证归档/当前文件并返回最终 hash，
从而支持有序多文件校验。

崩溃边界仍需注意：Audit 记录 fsync 与状态文件替换属于两个文件系统事务。
如果在两者之间崩溃，可能出现“持久化记录比状态锚更新”的情况。该狭窄窗口的恢复/协调属于下一步加固；
实现并不宣称两个文件之间具备原子性。

## Audit 检查点 v1 与前向恢复

状态文件现在是带版本的检查点，包含 sequence、字节偏移、chain hash 和 CRC32。
启动时直接 seek 到检查点 offset，从该处向前验证完整记录，并把检查点推进到持久日志尾部。
最后一条没有换行的部分记录视为崩溃残留，会截断到最后一个已验证 offset；
如果完整记录损坏，启动会以 EBADMSG 失败。

Audit log 仍是真实来源，检查点只是派生状态。检查点更新继续使用
“临时文件/fsync/re名称/目录 fsync”的原子替换方式。

当时的限制是自动协调只覆盖当前 Audit 文件。轮转过程中崩溃后，跨时间戳归档的完整发现与顺序验证
仍然需要单独实现。

## 轮转恢复阶段

启动恢复现在能够发现使用时间戳命名的 Audit 归档，根据文件名中嵌入的 UTC 时间排序，
并处理一个关键场景：检查点 offset 指向的字节已经因为轮转而从活动文件移动到了归档中。
恢复会先在归档历史中定位检查点 hash，从该点继续验证，然后再从头验证活动文件。

完整记录只要 `prev` 或 `hash` 损坏，仍然失败关闭。只有活动文件末尾“不以换行结束”的残片
才有资格作为崩溃残留被截断；归档文件绝不会被静默截断。

一次恢复扫描最多发现 4096 个匹配归档；生产部署应使用已有保留策略限制归档数量。

## 内部架构重构

公开 API 不变，但 Audit 实现职责已经拆分：
- `audit.c`：生命周期、事件编码、ATTEMPT/RESULT 语义和严格写入；
- `audit_integrity.c`：SHA-256/hash 原语；
- `audit_recovery.c`：检查点持久化、归档发现和崩溃恢复；
- `audit_verify.c`：离线验证 API；
- `audit_internal.h`：仅包含私有契约。

这是纯内部重构：应用仍然只需要包含 `audit.h`、`logger.h` 和 `console.h`。

## Logger 内部拆分——阶段 1

运行时格式化逻辑隔离到 `logger_format.c`，私有契约位于 `logger_internal.h`。
公开 `logger.h` API 不变。这一阶段有意不移动 MPSC 或文件后端状态；并发敏感代码按步骤拆分，
确保每次提取都先通过回归验证。

## Logger 内部拆分——阶段 2

有界 MPSC 序列环形队列原样移动到 `logger_queue.c`，并使用私有 `logger_queue.h`。
队列初始化、入队、单消费者 pop、批量排空和 empty 检查现在与 Logger 生命周期/后端代码隔离。
重构期间有意不修改算法和 memory ordering 选择。

## Logger 内部拆分——阶段 3

文件 open/write/writev、时间戳轮转、保留策略、重新打开、offset 和 fsync 行为隔离到
`logger_file.c`，通过私有 `logger_file_t` 封装。公开 API 保持不变。

## Logger 内部拆分——阶段 4

消费者/后端输出移动到 `logger_worker.c`。`logger_record.h` 现在拥有私有不可变记录结构，
用于打断私有头文件依赖环。`logger_internal.h` 维护核心与工作线程共享的私有 `struct logger` 契约。
`logger.c` 越来越集中于生命周期、记录捕获、公开 API、上下文和全局门面。

## Logger 内部拆分——阶段 5

进程全局便利门面（`logger_init`、`LOG_*`、全局刷新、级别、重新打开和指标）
已经隔离到 `logger_global.c`。核心 `logger.c` 不再拥有单例生命周期锁/指针。
私有可变参数日志入口在 `logger_internal.h` 中显式声明；公开 API 和宏用法保持不变。

## Fork 契约（当前）

初始化之后如果要执行受控单线程 fork/继续执行，使用 `logger_fork_reinit()`，
并在两个分支中分别显式初始化。辅助接口会在 fork **之前**由父进程拆除可选默认 Logger；
显式实例和 Audit 必须已经由各自所有者关闭。仍存在额外线程/实例时直接拒绝。
详见 `docs/CONTROLLED_FORK_REINIT.md` 与 `examples/fork_reinit.c`。

初始化后的原始 fork 继续使用第 5 轮 ECHILD 防护。子进程不会重置 mutex/rwlock，
也不会自动恢复任意继承状态。旧 prepare/父进程/子进程辅助接口不是新的受控操作。

## 生命周期状态机

`logger_t` 现在具有内部状态机，并允许通过公开接口查询：
CREATED -> RUNNING -> STOPPING -> STOPPED。
显式实例所有者必须在销毁前停止/等待所有使用者退出。历史上尝试过的活动调用计数方案
没有用于指针回收。销毁过程会先排空/停止工作线程，再拆除后端。

进程全局门面仍通过自己的 rwlock 提供更强的生命周期保证：并发 `LOG_*` 生产者可以与
`logger_shutdown()` 竞争，而不会访问已经释放的 Logger。显式 `logger_t *` 的所有权规则仍要求
`logger_destroy()` 返回后调用方不能再调用任何实例 API；不存在某个 C API 能让已经 free 的裸指针继续有效。

### 生命周期所有权澄清

曾尝试的活动调用固定计数器被有意放弃作为回收机制：裸 `logger_t *` 调用方可能恰好在
“观察到计数为 0”和 free 之间进入，因此单靠计数器无法让任意销毁后指针使用变得安全。
全局门面使用生命周期 rwlock，可以正确处理关闭/write 竞争。
显式实例使用状态机拒绝在 STOPPING 期间观察到的调用，但所有者仍然必须在
`logger_destroy()` 返回前等待/停止所有使用者。

### 关闭准入门

全局生产者获取生命周期读锁前会先检查关闭准入标志。
`logger_shutdown()` 会在请求写锁之前先关闭准入。这可以避免在不保证写者优先的 rwlock 实现中，
持续生产者流量导致关闭长时间饥饿。

## 故障注入验证框架

私有确定性故障钩子覆盖文件 write/fsync/re名称/ftruncate，以及 Audit 检查点
write/fsync/re名称。现在这些钩子 **只**编译进 `logger_test_support`。
生产 `liblogger.a` 即使环境变量存在也会忽略，并且不包含崩溃钩子代码。
独立测试归档使用 `LOGGER_FAULT_POINT/AFTER/ERRNO` 和短写钩子；只有指定故障/崩溃测试链接该库。

第一组故障矩阵验证严格同步 Logger 错误传播以及 Audit 初始化/检查点故障传播。
后续崩溃进程测试可以直接瞄准精确持久性边界，不再依赖不可重复的机器故障。

## 确定性崩溃点矩阵

故障验证现在会在持久性边界让真实子进程 `_exit(197)`，然后执行全新的 Audit 初始化和完整链验证。
覆盖边界包括：Audit 记录已 fsync 但检查点尚未更新、检查点 re名称 之前、
检查点 re名称 之后，以及检查点已提交但内存链头尚未更新。

这些测试使用真实进程退出验证重启协调，而不是只让系统调用返回 EIO。

## Sanitizer 检查验证

CMake 支持 `LOGGER_SANITIZE=address`、`undefined`、`address-undefined` 或 `thread`。
并发压力测试使用 12 个生产者，同时执行级别修改、指标读取和刷新，
异步工作线程同时排空 MPSC 队列。显式实例所有权得到遵守：销毁前会等待所有使用者退出。

## Sanitizer 检查器运行时诊断

执行环境使用 `readelf` / `ldd` 诊断。ASan 二进制链接插桩本身正确，但当时宿主测试框架
在 libasan 之前加载了另一个库。CMake 因此增加 `LOGGER_ASAN_PRELOAD`，让 CTest 运行时
可以把 libasan 放在最前面，而不改变生产链接关系。

另有独立 TSan 探测，用于区分“Logger 自身存在数据竞争”和“TSan 运行时/平台本身不可用”。
在该环境中解释 Logger TSan 失败前，必须先考虑探测结果。

## 公开 API 契约评审

`API.md` 现在记录 v1 候选契约：所有权、线程模型、fork、错误模型、Audit 持久性和 Console 通道语义。
仅实现内部使用的文件 offset 访问接口已从公开头文件移除并改为私有。
Audit 增加 `audit_instance_id_copy()`，调用方无需保留指向可变进程全局存储的指针。

## 测试配置

CTest 按用途使用标签：`unit`、`integration`、`concurrency`、`fork`、`fault`、`crash`、
`reliability`、`security`、`crypto`、`fast`。
`check-fast`、`check`、`check-production` 构建目标以及 `scripts/check.sh`
提供稳定的本地/CI 入口。详见 `TESTING.md`。

## 基准矩阵

`bench_matrix` 与 `scripts/benchmark.sh` 提供可重复的
1/2/4/8/16/32/64 个生产者 × 64/256/1024/4000 字节消息矩阵。
结果以 CSV/JSONL 输出生产者吞吐、端到端吞吐、丢弃数、队列 HWM、消费者记录数和批次数。
详见 `BENCHMARK.md`。
