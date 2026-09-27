# 可观测性与问题发现

## 目标

c-logger 的可观测性接口用于**发现和定位运行时退化**，不是再生成一层内部日志。

设计原则：

- 观测不能主动 flush、reopen、reconnect 或写任何 backend；
- unified diagnostics 不获取 `emit_mu`，避免文件/stdio/syslog 正在卡顿时连诊断线程也被拖住；
- 只读取 atomic/immutable/queue diagnostic state；
- 不在库内写死“70% 就告警”一类业务阈值；
- counter 表示从实例创建以来的累计事实，宿主应使用相邻快照 delta 做速率/告警；
- live queue/worker 字段是 race-free 的近似快照，不是事务一致性证明。

## 接口分层

### logger_get_diagnostics

```c
logger_diagnostics_t d;
logger_get_diagnostics(logger, &d);
```

显式实例的统一低开销快照。

包含：

- 实例 state / level / outputs；
- async / worker running / consumer waiting；
- drop、sync fallback、enqueued、dequeue、completion；
- emitted / failed / sticky first_error；
- queue capacity / current depth / lifetime high watermark；
- completion backlog；
- queue 预分配内存；
- spill capacity / current in-use / exhaustion；
- worker wait / producer wake / force wake；
- objective observation flags。

不会：

- 获取 backend output mutex；
- 执行文件、stdio、socket I/O；
- flush；
- reconnect Syslog；
- 清除 sticky error。

调用者仍必须保证 `logger_t *` 生命周期，不能与 destroy 并发。

### logger_get_global_diagnostics

```c
logger_diagnostics_t d;
logger_get_global_diagnostics(&d);
```

通过 global generation/lifetime pin 获取同样的快照。

STARTING/STOPPING/关闭 generation 的拒绝规则与现有 global metrics 一致，不会为了读取
diagnostics 越过 admission gate 或借到新的 generation。

### 现有专项接口

统一 diagnostics 不替换更细的专项接口：

- `logger_get_metrics()`：稳定旧 queue/drop metrics ABI；
- `logger_get_io_metrics()`：completion/output accounting；
- `logger_get_syslog_metrics()`：Syslog 一致 snapshot；该接口需要 `emit_mu`，
  因为 Syslog backend metrics 不是 atomic；
- `audit_get_status()`：Audit state、checkpoint、committed/checkpoint seq；
- `logger_flush_instance_status()`：主动同步与 sticky error 返回，不是纯观测。

## observation_flags

flags 只表示客观事实，不表示业务严重等级。

| flag | 含义 |
|---|---|
| `LOGGER_DIAG_DROPS_OBSERVED` | 实例生命周期内发生过 queue-full DROP |
| `LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED` | 发生过 queue pressure 导致的同步回退 |
| `LOGGER_DIAG_QUEUE_SATURATED` | queue high watermark 曾达到 capacity |
| `LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED` | long-message spill pool 至少耗尽过一次 |
| `LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED` | backend record failure 或 sticky first_error 已出现 |
| `LOGGER_DIAG_WORKER_STOPPED` | async instance 仍为 RUNNING，但 worker running flag 已关闭；这是 invariant 异常 |

除 `WORKER_STOPPED` 外，大部分 flag 是 lifetime-sticky observation：问题恢复后仍保持，
便于低频采样不漏掉短暂故障。

## 用快照发现问题

### 1. 日志丢失

观察：

```text
delta(dropped_records) > 0
```

含义：queue 无法接纳记录且该 level 的 policy 是 DROP。

同时看：

- `queue_depth / queue_capacity`
- `queue_high_watermark`
- `completion_backlog`
- `spill_exhaustions`

用于区分普通 queue saturation 与 long-message spill pressure。

### 2. async queue 压力

观察：

```text
delta(sync_fallbacks) > 0
LOGGER_DIAG_QUEUE_SATURATED
```

同步回退不是 drop，但说明 producer 已经碰到 async 容量边界，并把压力转移到 caller。

若同一时段：

```text
queue_depth 持续高
completion_backlog 持续增长
```

说明 consumer/backend 吞吐小于 producer 进入速率。

不要只看瞬时 depth：burst 后很快回落的高水位与持续积压是两个不同问题。

### 3. backend 卡顿而不是 queue 消费慢

`completion_backlog` 是：

```text
accepted async records - async completed records
```

`queue_depth` 只表示仍占 queue reservation/slot 的近似数量。

如果连续采样出现：

```text
completion_backlog 明显大于 queue_depth
```

说明部分记录已经被 worker dequeue，但 backend emit 尚未完成；应继续检查文件系统、
stderr consumer 或 Syslog，而不是只扩大 queue。

### 4. 长日志突发

观察：

```text
delta(spill_exhaustions) > 0
spill_in_use 接近 spill_capacity
```

这说明 >512B message 的预分配 spill pool 发生压力。

它与 queue capacity 独立：默认大 queue 也只有有限 spill blocks。不要把它误判为普通
short-message queue full。

### 5. 输出故障

观察：

```text
delta(failed_records) > 0
first_error != 0
LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED
```

`first_error` 是 sticky errno，后续成功不会清除。

若启用了 Syslog，再读取 `logger_get_syslog_metrics()` 区分：

- backpressure；
- cooldown rejection；
- connect failure；
- reconnect；
- close failure。

文件 fsync/reopen/rotation 错误不应通过“之后又成功写了一条”被视为已消失。

### 6. worker/wakeup 异常

正常空闲 async logger 常见：

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

单个快照可能只是 producer publish / signal 的短暂窗口，不能直接判 bug。

`LOGGER_DIAG_WORKER_STOPPED` 在 state=RUNNING 的 async instance 上不应出现；
出现时应作为实现 invariant failure 处理。

`worker_wait_count`、`producer_wake_signals`、`force_wake_signals` 用于分析 self-paced
wakeup 行为，不能简单把 wait 次数高解释成问题。

## 建议采样方式

宿主可每 1–10 秒取一次 snapshot，并保存前一份：

```c
logger_diagnostics_t now = {0};
logger_get_diagnostics(log, &now);

uint64_t dropped_delta = now.dropped_records - prev.dropped_records;
uint64_t failed_delta = now.failed_records - prev.failed_records;
uint64_t spill_delta = now.spill_exhaustions - prev.spill_exhaustions;
```

counter 只在同一个 logger instance 生命周期内做 delta。destroy/recreate 后必须重置 baseline。

推荐导出为宿主现有 telemetry 系统中的 counter/gauge，而不是让 c-logger 自己绑定
Prometheus、OpenTelemetry 或特定 agent：

- counter：drops / fallback / spill exhaustion / failed / emitted / wake；
- gauge：queue depth / completion backlog / spill in-use / worker state；
- sticky attribute/event：first_error / observation_flags。

这样库保持零外部 telemetry 依赖，宿主仍可接 Prometheus、OTel、StatsD 或自己的监控系统。

## 不能从这些指标得出的结论

这些 snapshot 不能证明：

- 日志已经被远端 Syslog 持久化；
- 文件已穿透 controller/device volatile cache；
- Audit 没有被拥有主机权限的攻击者整体重算/回滚；
- 某一个业务操作与日志记录构成原子事务；
- 单个瞬时 queue depth 就表示性能故障。

可靠性结论仍需要结合 status API、crash/power-cut evidence 和实际部署环境。
