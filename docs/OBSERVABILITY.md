# 可观测性与问题发现

> 现行设计入口见 [v2/README.md](v2/README.md)。本文保留的 Audit 专属名称、结构与行为仅为历史背景；
> Audit/密码学模块已退役，不属于当前 API、对象图或验证范围。普通 Logger 条款仍适用。

## 目标

c-logger 的可观测性接口用于**发现和定位运行时退化**，不是再生成一层内部日志。

设计原则：

- 观测不能主动 flush、重新打开、重连或写任何后端；
- 统一诊断不获取 `emit_mu`，避免文件/stdio/syslog 正在卡顿时连诊断线程也被拖住；
- 只读取原子/不可变/队列诊断状态；
- 不在库内写死“70% 就告警”一类业务阈值；
- 计数器表示从实例创建以来的累计事实，宿主应使用相邻快照差值做速率/告警；
- 运行中的队列/工作线程字段是无数据竞争的近似快照，不是事务一致性证明。

## 接口分层

### logger_get_diagnostics

```c
logger_diagnostics_t d;
logger_get_diagnostics(logger, &d);
```

显式实例的统一低开销快照。

包含：

- 实例状态 / 级别 / 输出目标；
- 异步 / 工作线程运行 / 消费者等待；
- 丢弃、同步回退、入队、出队、完成；
- 成功输出 / 失败 / 已锁存的 first_error；
- 队列容量 / 当前深度 / 生命周期内最高水位；
- 待完成积压；
- 队列预分配内存；
- 溢出块容量 / 当前使用量 / 耗尽；
- 工作线程等待 / 生产者唤醒 / 强制唤醒；
- 客观观测标记。

不会：

- 获取后端输出互斥锁；
- 执行文件、stdio、套接字 I/O；
- flush；
- 重连 Syslog；
- 清除已锁存错误。

调用者仍必须保证 `logger_t *` 生命周期，不能与销毁并发。

### logger_get_global_diagnostics

```c
logger_diagnostics_t d;
logger_get_global_diagnostics(&d);
```

通过全局代次/生命周期固定保护（pin）获取同样的快照。

STARTING/STOPPING/关闭代次的拒绝规则与现有全局指标一致，不会为了读取
诊断越过准入门禁或借到新的代次。

### 现有专项接口

统一诊断不替换更细的专项接口：

- `logger_get_metrics()`：稳定旧队列/丢弃指标 ABI；
- `logger_get_io_metrics()`：完成/输出记账；
- `logger_get_file_metrics()`：文件输出端一致快照；需要 `emit_mu`，但不执行任何 I/O；
- `logger_get_global_file_metrics()`：通过全局代次固定保护读取同一文件快照；
- `logger_get_syslog_metrics()`：Syslog 一致快照；该接口需要 `emit_mu`，
  因为 Syslog 后端指标不是原子变量；
- `audit_get_status()`：Audit 状态、检查点、已提交/检查点序号；
- `logger_flush()`：主动同步与已锁存错误返回，不是纯观测。

<a id="file-backend-metrics"></a>

### 文件后端指标

`logger_file_metrics_t` 用于把“输出失败”继续拆成文件后端的具体阶段：

| 字段 | 含义 |
|---|---|
| `write_operations` | `logger_file_write/writev` 顶层请求数；批次 writev 算一次 |
| `failed_write_operations` | 顶层写请求最终返回失败；可由写入、轮转或同步导致 |
| `write_syscalls` | 实际 `write/writev` 系统调用次数，包含 EINTR 重试 |
| `interrupted_write_syscalls` | 被 EINTR 中断并重试的写系统调用 |
| `failed_write_syscalls` | 非 EINTR 终止错误或零长度返回 |
| `bytes_written` | 实际成功写入的累计字节；最终操作失败前的部分字节也计入 |
| `data_sync_attempts/failures` | 活动文件/归档/新活动文件的数据 fsync；含 flush/轮转/重新打开内部同步 |
| `directory_sync_attempts/failures` | 目录持久化 fsync；含创建/轮换/保留策略/重新打开路径 |
| `rotation_attempts/successes/failures` | 内部按大小/日期轮转结果 |
| `reopen_attempts/successes/failures` | 显式重新打开结果 |
| `retention_deletions` | 保留策略成功 unlink 的历史归档数量 |
| `current_size` | 当前活动 fd 的已知累计大小 |
| `detached` | 活动文件已被重命名成归档，但新活动文件切换未完成 |
| `last_error` | 最近一次文件顶层操作结果；后续成功可清零 |
| `last_write_error` | 最近一次写操作失败 errno；后续成功不清历史 |
| `last_sync_error` | 最近一次数据/目录同步失败 errno；成功不清历史 |
| `last_rotation_error` | 最近一次轮转失败 errno；成功不清历史 |
| `last_reopen_error` | 最近一次重新打开失败 errno；成功不清历史 |

读取文件指标会短暂获取 `emit_mu` 来取得一致快照，但不会写入、fsync、重新打开、
轮转或清除任何错误。若后端正被真实文件 I/O 阻塞，文件指标读取也可能等待
该互斥锁；此时先用无锁（lock-free） `logger_get_diagnostics()` 判断待完成积压，再在
允许等待一致输出端快照时读取文件指标。

<a id="observation_flags"></a>

## 观测标记（observation_flags）

标记只表示客观事实，不表示业务严重等级。

| 标记 | 含义 |
|---|---|
| `LOGGER_DIAG_DROPS_OBSERVED` | 实例生命周期内发生过队列已满时的 DROP |
| `LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED` | 发生过队列压力导致的同步回退 |
| `LOGGER_DIAG_QUEUE_SATURATED` | 队列历史最高水位曾达到容量 |
| `LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED` | 长消息溢出块池至少耗尽过一次 |
| `LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED` | 后端记录失败或已锁存的 first_error 已出现 |
| `LOGGER_DIAG_WORKER_STOPPED` | 异步实例仍为 RUNNING，但工作线程运行标记已关闭；这是不变量异常 |

除 `WORKER_STOPPED` 外，大部分标记是生命周期内锁存的观测：问题恢复后仍保持，
便于低频采样不漏掉短暂故障。

## 用快照发现问题

### 1. 日志丢失

观察：

```text
delta(dropped_records) > 0
```

含义：队列无法接纳记录且该级别的策略是 DROP。

同时看：

- `queue_depth / queue_capacity`
- `queue_high_watermark`
- `completion_backlog`
- `spill_exhaustions`

用于区分普通队列饱和与长消息溢出块压力。

<a id="2-async-queue-压力"></a>

### 2. 异步队列压力

观察：

```text
delta(sync_fallbacks) > 0
LOGGER_DIAG_QUEUE_SATURATED
```

同步回退不是丢弃，但说明生产者已经碰到异步容量边界，并把压力转移到调用方。

若同一时段：

```text
queue_depth 持续高
completion_backlog 持续增长
```

说明消费者/后端吞吐小于生产者进入速率。

不要只看瞬时深度：突发后很快回落的高水位与持续积压是两个不同问题。

<a id="3-backend-卡顿而不是-queue-消费慢"></a>

### 3. 后端卡顿而非队列消费慢

`completion_backlog` 是：

```text
已接受的异步记录数 - 已完成的异步记录数
```

`queue_depth` 只表示仍占队列预留/槽位的近似数量。

如果连续采样出现：

```text
completion_backlog 明显大于 queue_depth
```

说明部分记录已经被工作线程出队，但后端输出尚未完成；应继续检查文件系统、
stderr 接收端或 Syslog，而不是只扩大队列。

### 4. 长日志突发

观察：

```text
delta(spill_exhaustions) > 0
spill_in_use 接近 spill_capacity
```

这说明 >512B 消息的预分配溢出块池发生压力。

它与队列容量独立：默认大队列也只有有限溢出块。不要把它误判为普通
短消息队列已满。

### 5. 输出故障

观察：

```text
delta(failed_records) > 0
first_error != 0
LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED
```

`first_error` 是已锁存的 errno，后续成功不会清除。

若启用了文件后端，读取 `logger_get_file_metrics()` 区分：

- write/writev 终止错误；
- 部分字节后失败；
- 数据 fsync 与目录 fsync；
- 轮转失败；
- 重新打开失败；
- 已分离的活动文件状态。

若启用了 Syslog，再读取 `logger_get_syslog_metrics()` 区分：

- 背压；
- 冷却期拒绝；
- 连接失败；
- 重连；
- 关闭失败。

文件的 `last_error` 会随后续成功清零，但 `last_write_error/last_sync_error/
last_rotation_error/last_reopen_error` 保留各类别最近一次失败；Logger 的 `first_error` 仍是实例级已锁存的
历史错误。不能通过“之后又成功写了一条”推断先前持久性失败已撤销。

<a id="6-workerwakeup-异常"></a>

### 6. 工作线程/唤醒异常

正常空闲异步 Logger 常见：

```text
consumer_waiting = 1
queue_depth = 0
```

压力下：

```text
consumer_waiting = 0
queue_depth > 0
```

需要关注的是**连续多个采样周期**出现：

```text
consumer_waiting = 1
queue_depth > 0
```

单个快照可能只是生产者发布 / 通知的短暂窗口，不能直接判缺陷。

`LOGGER_DIAG_WORKER_STOPPED` 在 state=RUNNING 的异步实例上不应出现；
出现时应作为实现不变量失败处理。

`worker_wait_count`、`producer_wake_signals`、`force_wake_signals` 用于分析自节奏
唤醒行为，不能简单把等待次数高解释成问题。

## 建议采样方式

宿主可每 1–10 秒取一次快照，并保存前一份：

```c
logger_diagnostics_t now = {0};
logger_get_diagnostics(log, &now);

uint64_t dropped_delta = now.dropped_records - prev.dropped_records;
uint64_t failed_delta = now.failed_records - prev.failed_records;
uint64_t spill_delta = now.spill_exhaustions - prev.spill_exhaustions;
```

计数器只在同一个 Logger 实例生命周期内做差值。销毁/重新创建后必须重置基线。

推荐导出为宿主现有遥测系统中的计数器/瞬时量，而不是让 c-logger 自己绑定
Prometheus、OpenTelemetry 或特定代理：

- 计数器：丢弃 / 回退 / 溢出块耗尽 / 失败 / 成功输出 / 唤醒；
- 瞬时量：队列深度 / 待完成积压 / 溢出块使用量 / 工作线程状态；
- 锁存属性/事件：first_error / observation_flags。

这样库保持零外部遥测依赖，宿主仍可接 Prometheus、OTel、StatsD 或自己的监控系统。

## 不能从这些指标得出的结论

这些快照不能证明：

- 日志已经被远端 Syslog 持久化；
- 文件已穿透控制器/设备易失缓存；
- Audit 没有被拥有主机权限的攻击者整体重算/回滚；
- 某一个业务操作与日志记录构成原子事务；
- 单个瞬时队列深度就表示性能故障。

可靠性结论仍需要结合 状态 API、崩溃/掉电证据和实际部署环境。
