# Lock Matrix

本表记录 c-logger 当前主要同步锁的职责、顺序与 guard 策略。修改锁图时必须同步更新。

| 锁 | 生命周期 | 保护对象/职责 | 允许的前置锁 | 是否允许继续拿锁 | guard 策略 |
|---|---|---|---|---|---|
| global `g_control_mu` | process-static | global init/shutdown controller | 无 | `g_lifetime_lock`，随后 instance lock | 保持显式 |
| global `g_lifetime_lock` | process-static | `g_logger` publish/destroy 与 read-side lifetime pin | `g_control_mu`（writer/controller）或无（reader） | instance lock | 保持显式 |
| `logger_t.emit_mu` | logger instance | file/syslog backend 可变状态、write/sync/reopen | global lifetime pin 或 Audit operation；普通 explicit API 可直接获取 | 不允许 `progress_mu/q.wait_mu` | 简单路径可 guard |
| `logger_t.progress_mu` | logger instance | `completed_pos/progress_cv` completion predicate | global lifetime pin；普通 explicit API 可直接获取 | 不允许 `emit_mu/q.wait_mu` | 可 guard |
| `logger_queue_t.spill_mu` | queue instance | long-message spill freelist | 无 | 不允许 `wait_mu/emit_mu/progress_mu` | 可 guard |
| `logger_queue_t.wait_mu` | queue instance | empty predicate + worker sleep/wakeup | 无 | 不允许 `spill_mu/emit_mu/progress_mu` | 可 guard |
| Console `g_console_mu` | process-static | Console FILE 输出与 flush | 无 | 无 | checked guard |
| Audit `g_control_mu` | process-static | Audit init/shutdown/controller | 无 | Audit `g_operation_mu` | 保持显式 |
| Audit `g_operation_mu` | process-static | runtime、seq/hash、transaction、in-flight drain | Audit `g_control_mu` 或无 | Audit private logger instance lock | 保持显式 |

## 不属于 pthread lock 的同步机制

以下机制不能混入 lock hierarchy 当作普通 mutex：

- `<active>.logger.lock` / Audit writer-lock fd：跨进程 ownership/exclusivity；
- `g_ticket`：generation + phase admission；
- queue slot `seq`：MPSC publication/reuse generation；
- `running`：worker stop state；
- `live_objects`：process census；
- refcount（当前 production 无实例）：独立 owner lifetime。

## 关键禁止关系

### instance locks 禁止嵌套

当前不允许 instance/queue 锁互相嵌套，包括：

```text
spill_mu -> wait_mu
wait_mu -> spill_mu
spill_mu -> emit_mu
emit_mu -> spill_mu
spill_mu -> progress_mu
progress_mu -> spill_mu
emit_mu -> progress_mu
progress_mu -> emit_mu
emit_mu -> q.wait_mu
q.wait_mu -> emit_mu
progress_mu -> q.wait_mu
q.wait_mu -> progress_mu
```

如果未来确实需要新增嵌套，必须先修改本表、补 deadlock 分析与并发测试。

### global 反向获取禁止

禁止：

```text
instance lock -> g_lifetime_lock
instance lock -> g_control_mu
g_lifetime_lock -> g_control_mu
```

global read-side API 必须先取得 lifetime pin，再进入 instance 操作。

### Console 独立

`g_console_mu` 不参与 Logger/Audit hierarchy。持锁时不要调用会重入 Console/Logger 的
host callback。

## condition variable 规则

`progress_cv` 必须与 `progress_mu` 配对。

`q.wait_cv` 必须与 `q.wait_mu` 配对。

`pthread_cond_wait()` 会原子释放并重新获取对应 mutex，因此使用 lexical guard 时，
guard 代表该 scope 对 mutex 的 ownership；wait 返回后 destructor 再负责最终 unlock。

## guard 迁移状态

本轮迁移：

- worker `emit_mu`；
- worker `progress_mu`；
- worker `q.wait_mu`；
- queue notify `q.wait_mu`；
- Console `g_console_mu`；
- explicit-instance 的 reopen/file-offset/flush/syslog-metrics 单锁路径。

刻意保留显式：

- logger teardown 中 join -> emit sync -> destroy mutex 的阶段；
- global control/lifetime 多锁；
- Audit control/operation 多锁与 cancellation-aware scope。

原因不是“guard 不支持”，而是这些路径的锁释放顺序本身就是 lifecycle/error contract。
