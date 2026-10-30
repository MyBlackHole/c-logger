# Audit 退役设计历史档案

> 本档案收录已退役 Audit 实现的五份历史设计文档。各来源正文按基线 `2f3b5bdb4b376fa3d4e0b53255df8e9b2ce1663f` 原样保留；这些内容用于追溯，不代表当前实现、支持范围或验证状态，也不恢复已退役功能。

## 来源索引

- [`AUDIT_FORMAT_FAILURE.md`：审计编码失败契约（#122）](#format-failure)
- [`AUDIT_RETIRE_OWNERSHIP.md`：审计运行时销毁失败的所有权契约](#retire-ownership)
- [`AUDIT_SEQUENCE_CONTINUITY.md`：Audit 序列与会话衔接验证](#sequence-continuity)
- [`AUDIT_TRANSACTION_SEMANTICS.md`：Audit 业务事务关联语义与验证边界](#transaction-semantics)
- [`AUDIT_TRANSACTION_SUBMISSION.md`：Audit 事务提交阶段与调用方事件所有权](#transaction-submission)

<a id="format-failure"></a>
## 原文件：`AUDIT_FORMAT_FAILURE.md`

<!-- DOC-001 SOURCE BEGIN AUDIT_FORMAT_FAILURE.md -->
# 审计编码失败契约（#122）

## 问题与范围

普通日志允许把时间转换失败显示为 `time-unavailable`，也允许有界截断。
严格审计不能承认这样的记录：当前校验器和恢复器要求合法完整的记录头。
本修复不改变持久格式、哈希域、公共结构体和符号，不放宽恢复器。

## 修改

`logger_log_sync_status()` 已检查正文编码；其 `force_sync` 输出路径现在还通过
`logger_format_line_checked()` 检查完整记录头和整行容量。返回 0/-errno：时间
转换或记录头表示范围失败为 `-EOVERFLOW`，格式编码失败为 `-EILSEQ`，非法
纳秒值为 `-EINVAL`。错误发生在任何输出后端之前，不会先写一个后端再失败。

普通 `logger_log()`、溢出同步回退和 worker 批量输出继续使用原有非严格路径。
未改变锁序、对象布局、队列算法或 worker 生命周期。本修改不解决 #123–#126。

时间缓存刷新前先失效；失败的部分字符串不能伪装为上一秒的有效缓存。
严格路径也验证缓存中的字节，防止普通日志填入超出审计语法的时间后被复用。
有效缓存命中不需要再次调用 libc 时间转换；故障只对实际执行的调用生效。

## 审计失败状态

Audit 继续沿用现有同步输出错误策略：`AUDIT_STATE_IO_FAILED` 是此版本已有的
输出失败状态，不新增 ABI 枚举。编码拒绝返回原错误，不推进 seq、链头或
checkpoint。停止会报告该错误且不伪造 STOP；恢复健康时间后可重新初始化。
这不表示已经尝试过磁盘 I/O，也不允许把没有输出的记录算作已提交。

## 验证

新增 `tests/regression/test_audit_format.c`，通过正常路径、缓存命中、
localtime_r 失败、strftime 日期/时区失败、非法日期/时区、跨普通日志共享缓存、
整行截断场景验证：返回值、状态、序号、checkpoint、日志及状态文件逐字节
不变、校验、停止、再次初始化与普通日志的降级行为。

`tests/regression/run_audit_format.sh BUILD_DIR [address,undefined]` 链接当前
checkout 已构建的生产静态库，不使用历史归档制品；脚本输出源码提交和库 hash。
核心 CI 在完整静态回归后运行，ASan/UBSan 任务链接同配置的已插桩生产库。
旧探针的“缺陷复现成功”与新回归的“正确行为通过”已分开。

同一生产归档还链接 `test_format_checked.c`，验证所有不足容量、恰好容纳
完整行、与普通格式化的成功输出一致、vsnprintf 编码失败、非法纳秒值和
错误后的再次调用。测试以错误返回、长度为零且不暴露部分输出作为断言。

本契约不宣称完整库无其他错误，不替代 #103/#104/#105 的剩余验收。
<!-- DOC-001 SOURCE END AUDIT_FORMAT_FAILURE.md -->

<a id="retire-ownership"></a>
## 原文件：`AUDIT_RETIRE_OWNERSHIP.md`

<!-- DOC-001 SOURCE BEGIN AUDIT_RETIRE_OWNERSHIP.md -->
# 审计运行时销毁失败的所有权契约

## 目的

审计子系统使用同步的私有 Logger，并持有审计日志和检查点的文件租约。
`audit_runtime_t` 是这些资源的结构性所有者，由 Audit 控制器管理。
Logger 的 `logger_destroy_status()` 返回非零并不能单独证明其对象已释放：
文件同步、`close()` 可以在实际最终释放之后报告错误；反之，同步对象
销毁、作用域或生命周期证明失败时，Logger 会保留其存储，禁止后续访问已
销毁的依赖资源。

本修复沿用原有宿主独占、SDK 借用模型及现成的**私有最终释放回执**
`logger_destroy_receipt_reset/read()`，不引入引用计数或通用工作队列，
不改变公开 ABI、日志格式或哈希链。

## 两种关闭结果

| 最终释放事实 | 处理方式 | Audit 后续状态 |
|---|---|---|
| Logger 已最终释放，无论是否伴随日志同步错误 | 依次关闭剩余 Audit 文件租约，释放运行时内存；按原错误向上报告 | IDLE，允许正常重新初始化 |
| Logger 未证明完成最终释放 | 不关闭依赖文件租约、不释放运行时、不重试部分销毁的 Logger | STOPPING（不可恢复），禁止重新初始化和重复关闭 |

`g_retired` 是由 `g_control_mu` 保护的唯一保留所有权位置，不是给所有
请求共享的引用计数；它保存一个未发布、未完成销毁的运行时。第一次
生命周期证明失败将原始错误锁存于 `g_retire_error`。该错误可通过
`audit_get_status()` 返回的 STOPPING/`error_code` 读取，无需取得
可能已经失效的操作锁。审计普通写入和检查点刷新仍由 STOPPING 准入门禁拒绝；
`audit_failure_policy()` 始终返回拒绝策略。

控制器下一次收到 `audit_init()` 或 `audit_shutdown_status()` 时，在
`g_control_mu` 下检查 `g_retired` 并直接返回锁存错误。不得因为
`g_runtime == NULL`、某次同步正常返回、或移除故障注入，就推断旧资源
已经回收；故障闭锁直到进程退出。

## 关键步骤

```text
拥有控制锁 → 关闭新调用准入 → 等待已有操作退出
   → 在控制器下卸载已发布 Audit runtime
   → 调用私有 Logger 销毁（预先重置最终释放回执）
       ├── 无最终释放回执
       │     → g_retired 持有完整 runtime（Logger、日志租约、检查点租约）
       │     → 锁存错误、保持 STOPPING、禁止重试/重新初始化
       └── 确认最终释放
             → 关闭所有剩余 Audit 文件租约
             → free(runtime)
             → 记录旧错误、进入 IDLE、允许以后初始化
```

初始化候选创建过程中，即使 `AUDIT_START` 写入失败或初始化发布阶段的
操作锁获取失败，也通过同一销毁路径处理候选对象；若候选的私有 Logger
未成功完成最终释放，同样将其交给 `g_retired`，而不是恢复 IDLE。

同步 Audit 私有 Logger 本身**没有异步工作线程**，因此本轮实际故障注入
针对条件变量/互斥锁销毁失败，以及链接时模拟的未完成最终释放；不把
模拟失败伪装成真实的 Audit worker join 错误。

Linux 文件描述符的 `close()` 失败不会通过再次关闭相同整数 fd 进行
重试；内部关闭函数会将所有本地 fd 位置置为 -1 并依次进行关闭。
这种文件关闭错误与仍可能有活跃借用者的 Logger 生命周期证明失败必须区分。

## 故障注入验证

`audit_lifecycle_regression` 在既有线程交错回归外新增六个独立场景：

- `dispose-cond`：私有 Logger 的条件变量销毁失败；保留完整运行时与写锁租约
- `dispose-mutex`：私有 Logger 的互斥锁销毁失败；保留完整运行时与写锁租约
- `dispose-no-final-release`：链接包装器模拟没有进入最终销毁；不能把错误返回当成已释放证明
- `init-rollback-retain`：`AUDIT_START` 写入失败后候选回滚又发生同步对象销毁失败；禁止新初始化
- `close-error-finalized`：租约已经实际关闭后才模拟返回 I/O 错误；仍可重新初始化
- `teardown-failure-race`：关闭控制器在 Logger 最终释放阶段暂停，另一个初始化线程竞争控制锁；失败后双方必须看到同一永久闭锁

故障用例检查进程级对象登记、第二次销毁次数、文件租约仍被占用、
`audit_get_status()`、业务拒绝策略，以及允许重新初始化的对照分支。
它们通过链接期包装注入，不向生产路径添加测试钩子，也不以
自动屏蔽泄漏检测代替真实的所有权保留。

## 边界

保留对象意味着该进程可能持续占用文件描述符、锁和内存；
这是一条**无法证明安全回收时宁可保留资源也不发生悬挂访问**的终态，
不保证当前进程能够恢复服务。恢复操作应在宿主完成其他资源控制后，
通过新的进程生命周期处理。不可使用旧指针重试部分销毁对象。

本次未改变审计的哈希链、检查点持久化顺序、磁盘恢复格式或并发工作模型。
进一步的崩溃恢复证明独立跟踪于 #105。
<!-- DOC-001 SOURCE END AUDIT_RETIRE_OWNERSHIP.md -->

<a id="sequence-continuity"></a>
## 原文件：`AUDIT_SEQUENCE_CONTINUITY.md`

<!-- DOC-001 SOURCE BEGIN AUDIT_SEQUENCE_CONTINUITY.md -->
# Audit 序列与会话衔接验证

## 范围

本契约适用于当前生产记录格式中 `instance`、`seq`、`prev`、
`hash` 的语义验证。它不会改变磁盘字节、哈希算法、公开 ABI、
日志事务的独立提交边界，也不会要求归档名按时间排序。

生产 Audit 会话在 `audit_init()` 后使用新的随机 128 位
`instance_id`；本次会话内的 `seq` 从 1 递增。
同一会话的文件轮换不会重置序号；重新初始化才会创建新会话。
新会话的首条记录是 `phase=RESULT`、`event="AUDIT_START"`、
`seq=1`。由于失败会话可以在没有成功 `AUDIT_STOP` 的情况下
结束，**不以出现 STOP 作为新会话的必要条件**。

## 最小充分校验规则

逐条成功解析、验证哈希链接后，用 `audit_sequence_advance()`
推进私有的 `instance + seq` 游标：

1. 同一实例：下一条的 `seq` 必须严格等于上一条加 1；
   `UINT64_MAX` 之后不得回绕。
2. 实例标识变化：下一条必须是规范序列化的
   `AUDIT_START` 且 `seq=1`。
3. `AUDIT_START` 自身必须是 `seq=1`，不能插入已在进行的会话。
4. 对于从零哈希完整起源（genesis）开始的恢复链，第一条
   **必须是字段全部合法的 `AUDIT_START`，且 `seq=1`**；
   无显式锚点的离线验证也遵循此规则。
5. 有明确检查点锚点的归档保留尾段，第一条可能是本会话
   `seq > 1` 的普通业务记录或合法的 `AUDIT_STOP`。
   **仅在验证者无法看到其前驱时**允许这一前缀；后续记录仍必须连续。
6. `AUDIT_STOP` 结束本实例。它后面只能是**不同实例的合法
   `AUDIT_START`**，不能再出现该实例的普通事件或第二条 STOP。
7. 恢复器必须先按摘要 `last_hash → first_prev` 得出真实归档顺序，
   再比较相邻归档的 `last instance/seq` 与 `first instance/seq`。
   不允许用文件名、墙上时间或目录枚举顺序推断写入顺序。

同一个内部校验函数同时用于：
- 恢复器的单段扫描；
- 恢复器在已经确定哈希邻接关系后验证跨归档边界；
- 离线校验器读取同一文件的所有记录。

新的校验基于已经存在的字段，记录缓冲区依旧固定有界；
恢复器增加少量堆分配的归档边界摘要，不使用通用全历史索引，
也不增加无界栈缓冲区。

## 系统起止记录的固定属性

解析器区分“语法上能够完整解析”和“系统事件符合生产写入契约”。
旧语法解析仍可接受哈希正确但字段被伪造的记录，用于明确报告语义错误。
任何名为 `AUDIT_START` 或 `AUDIT_STOP` 的记录都不得伪装成普通业务事件。

| 字段 | `AUDIT_START` | `AUDIT_STOP` |
|---|---|---|
| `phase` | `RESULT` | `RESULT` |
| `txn` | `0` | `0` |
| `seq` | `1` | 大于 `1` |
| `actor` | `"system"` | `"system"` |
| `source` | `"local"` | `"local"` |
| `resource` | 非空且属于生产允许字符集的审计会话名称 | `"audit"` |
| `operation` | `"audit_start"` | `"audit_stop"` |
| `result` | `SUCCESS` | `SUCCESS` |
| `error` | `0` | `0` |
| `detail` | 不允许出现 | 不允许出现 |

系统事件名和固定字段采用生产输出的规范原始表示，不引入另一套
转义等价规则。系统事件的哈希、会话标识、序号、跨归档边界必须
分别通过验证，不能用其中任何一项替代其他证明。

正常关闭最后写 `AUDIT_STOP`；异常关闭可能没有 STOP，此时允许
下一条哈希衔接的记录由**新实例 ID、合法 START、序号 1**继续。
保留的历史尾段即使从 STOP 开始，也必须由合法检查点锚定。

## 典型场景

```text
实例 A（轮换无关）：
    A/seq=1  →  A/seq=2  →  A/seq=3            合法
    A/seq=1  →  A/seq=3                       拒绝：缺号
    A/seq=1  →  A/seq=1                       拒绝：重复
    A/seq=2  →  A/seq=1                       拒绝：相同实例回退

实例切换：
    A/seq=2  →  B/seq=1  AUDIT_START          合法
    A/seq=2  →  B/seq=1  EVENT                拒绝：无会话起点
    A/seq=2  →  B/seq=2  AUDIT_START          拒绝：新会话起始不是 1

归档衔接：
    archive_1 [A/1,A/2] → archive_2 [A/3] → active [A/4]
    archive_1 [A/1,A/2] → archive_2 [B/1 AUDIT_START] → active [B/2]

有锚点的历史尾段：
    checkpoint 已确认 A/7 → 保留 archive [A/7,A/8] → active [A/9]
    允许从 A/7 重新扫描，但无锚点的完整 genesis 校验不会承认 A/7 为起点。
```

## 错误与安全属性

序列不连续或非法会话切换统一返回 `EBADMSG`。恢复器在确定整个
归档序列有效之前不应截断尾部、不推进检查点，也不更新调用方
`audit_ckpt_t`。离线 `audit_verify_file_from()` 在失败时
保持调用方的最终哈希输出不变。

测试采用每次修改后**重新计算正确 SHA-256** 的记录，
确保问题并非由原有 `hash` 校验偶然发现。
还需要区分以下边界：

- SHA-256 无密钥哈希链提供错误检测与链式一致性，
  **不能防止有权限的攻击者重写历史并重新计算整个哈希链**。
- 本次没有引入全历史 `instance_id` 去重索引；
  多个不相邻会话是否复用了曾出现的随机实例标识，不由此增量游标证明。
  正常写入方仍由 CSPRNG 产生新 ID。
- 本次不增加事务 BEGIN/END 配对验证，不把业务事务原子性、
  `AUDIT_STOP` 成功或外部时间顺序混同于序列连续性。
  新增的是对已存在 STOP 记录的**固定字段**和**同实例终止状态**校验。
- 离线检验从无锚点起源开始；要验证从中途提取的文件，应使用
  现有带前驱摘要的锚点接口。它同样无法从单个文件推断看不见的归档历史。

## 验证

新增 `audit_sequence_regression` 的十六项场景涵盖：
正常轮换、正常恢复起源、在归档边界与归档内部切换会话、
检查点锚定的历史尾段、同一会话的缺号/重复/跨归档缺号与重复、
实例改变但缺少 START、START 起始号不为 1、旧 ID 重置、
错误位置的 START、非法 genesis、整数溢出回绕。

每个异常场景同时断言：
- 所有记录的独立 SHA-256 哈希链接正确；
- 恢复器拒绝并保持入参检查点与文件字节不变；
- 离线校验器拒绝同样的序列；
- 合法链可反复恢复，恢复结果相同。

本轮继续增加 `audit_system_regression` 的三十二项场景，全部使用
合法语法和可独立验证的 SHA-256 哈希：测试固定字段伪造、错误
START/STOP 位置、同实例 STOP 之后追加、合法的异常关闭后重启、
跨归档 STOP/START 衔接，以及检查点锚定历史尾段。
正常生产日志、重复恢复、失败时原始文件及检查点保持不变，
均属于回归验收要求。
<!-- DOC-001 SOURCE END AUDIT_SEQUENCE_CONTINUITY.md -->

<a id="transaction-semantics"></a>
## 原文件：`AUDIT_TRANSACTION_SEMANTICS.md`

<!-- DOC-001 SOURCE BEGIN AUDIT_TRANSACTION_SEMANTICS.md -->
# Audit 业务事务关联语义与验证边界

## 结论

当前 Audit 格式中的 `txn` 和 `phase` 是**关联元数据**，不是一套可由恢复器证明的事务配对协议。当前实现验证记录语法、单条摘要、哈希链、会话序号和 `AUDIT_START/AUDIT_STOP` 生命周期；它**不保证**每个 `ATTEMPT` 都有且只有一个匹配的 `RESULT`。

不要把“哈希链有效”解释为“业务事务完整”。哈希链能检测未同步修改，但无密钥 SHA-256 不能阻止有权限的攻击者重写整条历史并重新计算摘要。

## 为什么现在不能强制配对

- `audit_write()` 接受单条事件；调用方可以直接提交 `ATTEMPT` 或 `RESULT`，不经过 `audit_begin()/audit_end()`。
- `audit_begin()` 和 `audit_end()` 是同步提交辅助接口，并不维护持久化的待完成事务注册表。调用方传入的事件对象仍由调用方拥有。
- 现有磁盘记录没有来源字段，恢复器无法区分记录来自 `audit_write()`、`audit_begin()` 还是 `audit_end()`。
- `transaction_id` 只标识调用方提供或本会话自动分配的关联值；现有格式不承诺全历史唯一性，也没有定义相同编号跨实例的全局含义。
- 进程崩溃可能留下只有 `ATTEMPT` 的日志尾部。该情况本身不能证明记录被破坏，更不能仅因缺少 `RESULT` 就拒绝整个日志。

因此，单纯在离线验证或恢复路径里加入“所有 ATTEMPT 必须配对 RESULT”的检查，会误拒绝当前接口允许产生的记录，并可能让历史日志无法恢复。单纯检测重复 `RESULT` 也会误判直接写入的合法记录。

## 当前验证保证

| 属性 | 当前是否保证 |
|---|---|
| 记录格式与字段编码有效 | 是 |
| 单条摘要及连续哈希链有效 | 启用 SHA-256 时是 |
| 同一会话序号连续、实例切换合法 | 是 |
| 系统 START/STOP 字段和终止状态合法 | 是 |
| 每个 ATTEMPT 都有 RESULT | 否 |
| RESULT 与先前 ATTEMPT 的业务字段一致 | 否 |
| transaction_id 在完整历史中唯一 | 否 |
| 仅凭哈希链识别完整重写的恶意历史 | 否 |

## 未来若要严格配对，必须先设计契约

严格事务校验不应只靠解析器猜测。实施前至少需要明确并冻结：

1. **记录分类**：如何在磁盘上区分事务协议记录与允许独立写入的普通事件。不能仅根据当前 `phase` 推断来源。
2. **配对键**：事务 ID 的作用域是单实例、整个归档链还是外部业务域；何时允许复用；开始记录的业务身份字段有哪些。
3. **结束规则**：一个事务是否允许多个阶段结果、失败后重试或重复报告；哪些重复是错误。
4. **崩溃规则**：尾部未完成事务是否是合法的待处理状态；是否要报告不完整事务，而不是将整条日志判为损坏。
5. **归档与检查点**：事务开始和结束可能跨文件；保留的锚定历史尾段可能看不到开始记录。验证器必须区分“无法看到前驱”与“证明前驱不存在”。
6. **资源上界**：如果验证全历史的唯一性或未完成事务集合，需要定义内存/磁盘索引上限、溢出处理和恢复时间，而不能悄悄引入无界映射。
7. **兼容迁移**：若需要新增磁盘字段或事件类型，应设计格式版本/迁移策略并为旧日志定义明确行为；不得在不改变格式的情况下声称可以可靠恢复来源信息。

在这些规则确定前，保持现有兼容行为，并明确暴露验证边界，比在恢复器中加入不完整的配对检查更安全。

## 回归验收方向

未来的严格配对实现应使用重新计算正确摘要的语义篡改样本，并覆盖正常配对、缺失结束、重复结束、字段不匹配、事务 ID 复用、崩溃尾部、跨归档配对、锚定尾段和资源上限。所有拒绝场景还应证明恢复失败时日志字节和调用方检查点均不被修改。
<!-- DOC-001 SOURCE END AUDIT_TRANSACTION_SEMANTICS.md -->

<a id="transaction-submission"></a>
## 原文件：`AUDIT_TRANSACTION_SUBMISSION.md`

<!-- DOC-001 SOURCE BEGIN AUDIT_TRANSACTION_SUBMISSION.md -->
# Audit 事务提交阶段与调用方事件所有权

## 适用范围

本文约束当前 `audit_begin()`、`audit_end()` 与同步 Audit 日志写入的交界。
它不引入新的公共结果枚举、公共结构体字段、磁盘格式、哈希算法或恢复 API。
Audit 仍由单写者和 `g_operation_mu` 串行化 `seq`、哈希链及事务号。

`audit_event_t` 是调用方拥有、Audit 可按契约更新的事件对象；它不是稳定的
持久化凭据。事件中的 `transaction_id` 标识一个尝试，不是提交成功证明。
`audit_get_status()` 返回会话状态快照，不是单个事务的独立提交回执。

## 提交阶段

内部 `write_runtime()` 为事务入口提供以下阶段：

| 阶段 | 已进入严格日志后端 | 日志已确认同步提交 | 调用方事件与事务号 |
|---|---|---|---|
| `AUDIT_NOT_SUBMITTED` | 否 | 否 | 保持调用前原样；自动事务号不消耗 |
| `AUDIT_SUBMISSION_UNCERTAIN` | 是 | **未获确认** | 写入本次尝试的事务号与阶段；自动事务号保留，不得当作已提交 |
| `AUDIT_RECORD_COMMITTED` | 是 | 是 | 写入已提交事件的事务号与阶段；即使后续检查点失败也是如此 |

其中“未获确认”不等于“确认未写入”。例如，`write()` 短写之后失败、
`fsync()` 返回错误，磁盘上可能存在部分甚至完整记录。
保守起见，即使后端在内部同步锁阶段返回错误，也按未确认处理，
而不是未经证明地归类成纯输入错误。

```text
业务事件（由调用方持有）
    ↓
校验 + 计算候选事务号（不修改调用方，也不推进 next_txn）
    ↓
编码 + 散列（未进入日志后端）
    ├── 错误 → NOT_SUBMITTED：对象原样；会话是否失败取决于错误类型
    ↓
调用强制同步的日志后端
    ├── 错误 → SUBMISSION_UNCERTAIN：保留尝试标识；锁定 IO_FAILED
    ↓
确认日志提交、推进 seq/hash
    ↓
COMMITTED：保留已提交的事件状态
    ↓
检查点提交
    ├── 错误 → CHECKPOINT_FAILED：记录不能重写；可修复 checkpoint
    └── 成功 → RUNNING
```

## 错误语义

- 参数无效、编码超长、序号/自动事务号溢出等写前错误，不会修改调用方的
  `audit_event_t`，也不会提前消耗自动分配的事务号。
- 散列引擎在输出前失败，同样不修改调用方事件，但会将会话锁定在
  `CRYPTO_FAILED`；这不是可直接继续写入的业务输入错误。
- 后端返回错误后，调用方能拿到本次**尝试**的事务号、`phase`、
  `result` 等候选字段，并通过 `audit_get_status()` 看到 `IO_FAILED`；
  `committed_seq` 不伪装成已确认提交。不能直接重试该条记录，
  必须关闭会话、重新打开并核对日志与恢复结果。
- 同步日志返回成功但 checkpoint 提交失败时，虽然 API 返回 `-1`，
  调用方事件已代表确认持久化的记录；`committed_seq` 前进，
  `checkpoint_dirty` 为真。现有 `audit_flush()` 只修复检查点，
  不会重写先前的已提交事件。
- `audit_end()` 的结果字段仅在事件进入后端时更新。失败前的
  `audit_begin()` 不会改变调用方原先的 `phase` 或 `transaction_id`。
- 审计会话重建后，旧会话事务不能直接跨代复用；本 PR 不扩大该 API 契约。

## 验证矩阵

`audit_commit_regression` 新增九个独立进程用例：

| 用例 | 核验 |
|---|---|
| `txn-preflight` | BEGIN/END 编码超长不修改对象或文件；下一次 BEGIN 的自动事务号仍从 1 开始 |
| `txn-begin-write` / `txn-end-write` | 首次系统调用写入失败；暴露尝试标识而不确认提交 |
| `txn-begin-partial` / `txn-end-partial` | 部分写后失败；无假提交、无盲重试 |
| `txn-begin-fsync` / `txn-end-fsync` | 写入后同步失败；进入 IO_FAILED 并要求恢复核对 |
| `txn-begin-checkpoint` / `txn-end-checkpoint` | 已提交记录保留事件状态；刷新只修复检查点；无重复记录 |

同时保留原有后台 I/O 不确定、Audit 会话、崩溃恢复、
文件轮换与现有真实 ABI 检查。九个新增场景均在 C11 用户态契约内，
通过已有链接期故障注入进行验证；不模拟不受支持的
跨进程调用者持有相同 `audit_event_t` 对象的无同步并发。
<!-- DOC-001 SOURCE END AUDIT_TRANSACTION_SUBMISSION.md -->
