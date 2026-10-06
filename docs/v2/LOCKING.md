# c-logger 2.0 锁与阻塞契约

状态：**#103 第一阶段设计基线**。

本文件定义 2.0 当前锁图。修改锁图、增加嵌套或改变 condition predicate 时必须同步更新。

## 1. pthread 锁矩阵

| 锁 | 保护对象/职责 | 允许前置锁 | 后续可取锁 | lifetime 作用 | 阻塞上下文 |
|---|---|---|---|---|---|
| Global `g_control_mu` | init/shutdown controller | 无 | `g_lifetime_lock`，随后 instance lock | controller exclusivity，不直接 pin object | MAY_BLOCK |
| Global `g_lifetime_lock` | `g_logger` publish/unpublish + reader lifetime pin | writer 可持 `g_control_mu`；reader 无 | instance lock | **是：当前 global reader lifetime pin** | MAY_BLOCK |
| `logger_t.emit_mu` | file/syslog backend mutable state、write/sync/reopen | global lifetime pin / Audit operation / explicit caller scope | 不允许 progress/wait lock | 否 | MAY_BLOCK |
| `logger_t.progress_mu` | `completed_pos` + `progress_cv` predicate | global lifetime pin / explicit caller scope | 不允许 emit/wait lock | 否 | MAY_BLOCK |
| `logger_queue_t.wait_mu` | worker sleep predicate + condvar handshake | 无 | 不允许 emit/progress lock | 否 | MAY_BLOCK，仅 slow path |
| Console `g_console_mu` | FILE output + flush | 无 | 无 | 否 | MAY_BLOCK |
| Audit `g_control_mu` | Audit init/shutdown controller | 无 | Audit `g_operation_mu` | controller exclusivity | MAY_BLOCK |
| Audit `g_operation_mu` | runtime pointer、seq/hash、transaction、in-flight drain | Audit `g_control_mu` 或无 | Audit private logger instance lock | **是：当前 Audit runtime borrow scope** | MAY_BLOCK |

## 2. 非 pthread-lock 同步机制

以下不能混进 mutex hierarchy：

- file `<active>.logger.lock` / Audit writer lock fd：跨进程 ownership；
- Global `g_ticket`：phase + generation admission；
- queue `slot.seq`：payload publication + reuse generation；
- queue `spill_used[]`：spill block ownership/reuse；
- `running`：worker stop state；
- `state/g_phase`：admission state；
- process `live_objects`：census；
- refcount：独立 owner lifetime（当前无 production object 使用）。

## 3. instance lock 禁止嵌套

当前禁止：

```text
emit_mu -> progress_mu
progress_mu -> emit_mu
emit_mu -> q.wait_mu
q.wait_mu -> emit_mu
progress_mu -> q.wait_mu
q.wait_mu -> progress_mu
```

worker 的流程必须拆成：queue consume -> emit -> progress update，而不是同时持多把 instance lock。

## 4. Global lock order

唯一允许的 controller 顺序：

```text
g_control_mu
    -> g_lifetime_lock (writer)
        -> instance operation
```

reader：

```text
g_lifetime_lock (reader)
    -> instance operation
```

禁止反向：

```text
instance lock -> g_lifetime_lock
instance lock -> g_control_mu
g_lifetime_lock -> g_control_mu
```

## 5. Audit lock order

controller：

```text
Audit g_control_mu
    -> Audit g_operation_mu
        -> private logger instance lock
```

ordinary operation：

```text
Audit g_operation_mu
    -> private logger instance lock
```

禁止 private logger callback/reentry 反向进入 Audit controller。

## 6. condition variable contract

`progress_cv` 必须与 `progress_mu` 配对；predicate 是 completion watermark。

`q.wait_cv` 必须与 `q.wait_mu` 配对；predicate 是 queue/running 状态。

所有 wait 都必须：

```text
lock mutex
while (!predicate)
    cond_wait
unlock mutex
```

signal/broadcast 只是通知，不是状态本身。

## 7. guard 使用边界

简单单锁 lexical scope 可以使用 `guard/ACQUIRE`。

以下保留显式控制流：

- join -> destroy -> free teardown；
- Global control/lifetime 多锁；
- Audit control/operation 多锁；
- cancellation restore 与锁释放顺序本身属于 contract 的路径。

## 8. lockdep-style assertions 的实现边界

不能通过对 raw normal `pthread_mutex_t` 调 `pthread_mutex_trylock()` 来假装实现 `lockdep_assert_held()`：

- 无法可靠区分“当前线程持有”和“其他线程持有”；
- normal mutex 的 self-try/owner 语义不是内核 lockdep；
- 可能改变程序状态或把 debug helper 变成同步操作。

2.0 如实现 lockdep-style assertions，应使用**显式 debug ownership tracking**，例如仅 test/debug build 记录：

```text
lock class
owner thread id
held depth
acquire sequence/order
```

production build 应零或极低成本。

第一阶段只冻结 lock matrix 和 assertion contract，不在本 PR 中重写所有 pthread mutex。

## 9. callback/reentry

持有以下锁时禁止调用未知 host callback：

- Global control/lifetime lock；
- logger instance locks；
- Audit control/operation locks；
- Console output lock。

Logger/Console 当前通过 TLS runtime scope 拒绝同线程直接重入；这不是跨线程 deadlock proof，也不是 signal-handler safety。

## 10. fork/cancellation

fork child 不允许接触继承来的 pthread lock state；process identity gate 必须先执行。

cancellation 恢复必须发生在全部 library lock/lifetime pin 释放之后。

`pthread_cond_wait()` 返回时 mutex 已重新获取，cleanup/guard 才能负责最终 unlock。

## 11. 锁变更 review

新增或修改锁必须说明：

1. protects what；
2. 执行上下文；
3. allowed predecessor；
4. allowed successor；
5. 是否参与 lifetime proof；
6. 是否跨 callback/I/O；
7. condition predicate；
8. cancellation/fork 影响；
9. 是否需要 lockdep-style assertion；
10. failure-path test。
