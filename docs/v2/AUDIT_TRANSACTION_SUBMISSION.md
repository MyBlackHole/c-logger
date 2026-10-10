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
