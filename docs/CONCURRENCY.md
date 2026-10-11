# 并发模型

c-logger 将生命周期机制与同步机制明确分离，二者不能视为可以互相替代：

```text
atomic ordering -> publication/individual atomic state
lock            -> multi-field invariant serialization
lifetime pin    -> object cannot be released while borrowed
join            -> thread/user lifetime has ended
refcount        -> independent owners count lifetime references
state machine   -> legal lifecycle transitions
```

## MPSC 队列发布协议

队列采用有界 MPSC 结构，每个槽位维护独立的序列值。

生产者：

```text
reserve enqueue position
    |
write complete compact queue record
    |
release-store slot.seq = published
```

消费者：

```text
acquire-load slot.seq
    |
if published: read payload
    |
release-store slot.seq = reusable generation
    |
advance dequeue position
```

`slot.seq` 周围的 release/acquire 配对负责发布记录内容。
使用 relaxed 的位置计数器只承担预留与记账职责；在没有完整审查整个协议前，不得随意提高或降低其内存序。

## 队列长消息溢出区协议

队列 slot 可内嵌最多 512B 正文。更长消息先从预分配 溢出区 pool
取得一个独占 block，再 reserve/发布 队列 slot。

发布 顺序为：

```text
producer 从 spill bitmap claim block（仅长消息）
    ->
写完整 spill text
    ->
reserve queue position
    ->
写 compact metadata + spill index
    ->
release-store slot.seq
```

消费者 通过 acquire-load `slot.seq` 后读取 compact record 与 溢出区 text，
复制到 工作线程 batch，然后在把 slot 标记为 reusable **之前**归还 溢出区 block。

溢出区 block 所有权 由固定 atomic bitmap 管理：生产者 通过 0->1 CAS claim，
消费者 完成复制后通过 1->0 atomic clear release。这里不再存在共享 溢出区 mutex。

溢出区 pool 暂时耗尽不允许截断 long message。队列 push 返回 unavailable，
上层继续使用原有 DROP / SYNC 溢出 policy。

## 队列自节奏唤醒协议

MPSC payload 发布 与 工作线程 sleep/wakeup 是两个独立协议。

当前使用同一个 `consumer_waiting` atomic 上的 acq_rel RMW handoff；旧双 SC fence 协议已移除。
初始化后所有并发 waiting 修改均为 acq_rel RMW，不能用 plain load/store 替换 helper。

工作线程 只有在 drain 发现 队列 为空后才进入 wait protocol：

```text
lock wait_mu
  -> exchange(consumer_waiting, 1, acq_rel)  // 准备等待
  -> recheck queue + running
  -> still empty/running ? cond_wait : do not sleep
  -> exchange(consumer_waiting, 0, acq_rel)  // 结束等待
  -> unlock wait_mu
```

生产者 完成 `slot.seq` release 发布后，执行 `fetch_or(consumer_waiting, 0, acq_rel)` probe：

```text
probe old == 0
    -> 直接返回，不碰 wait_mu；RMW 仍参与 publication handoff chain

probe old == 1
    -> lock wait_mu
    -> exchange(consumer_waiting, 0, acq_rel)
    -> old == 1 时 signal 一次；old == 0 时已有 resolver 处理该 wait cycle
    -> unlock wait_mu
```

同一 atomic 的 RMW modification order 给出两类情况：

- head producer 的 publish/probe 在 worker arm 之前：arm acquire 取得该 RMW chain，
  随后的 slot acquire recheck 必须看到 head publication；
- worker arm 在 head producer probe 之前：尚未被 resolver 清除时，probe 观察到 1，
  producer 持 `wait_mu` claim 后 signal；同一 mutex 与 cond_wait 的原子释放/等待衔接，防止 wait-entry 丢唤醒。

多个生产者的 RMW 构成传递链。若 probe 看到 intervening clear 后的 0，则该 wait cycle
已由 producer signal claim、worker disarm 或 force wake 处理，不能把任意 plain store 0 加入协议。
后续 slot 的 publication 不保证当前 dequeue head 已可消费；关闭 head reservation gap 的
producer 仍须执行自己的 publish/probe。

stop/shutdown 先关闭 `running`，再持 `wait_mu` 执行 acq_rel RMW disarm 并无条件 force signal。
完整 C11 证明与失败/进度边界见 [QUEUE_WAKEUP_PROOF.md](v2/QUEUE_WAKEUP_PROOF.md)；
litmus、wait-entry regression 与 TSan 不能替代 happens-before 证明。

private diagnostics：

- `wait_count`：实际进入 cond_wait 的次数；
- `producer_wake_signals`：生产者 真正发送的 唤醒信号 次数；
- `force_wake_signals`：shutdown/stop 强制 唤醒信号 次数。

这些计数只在真实 wait/唤醒信号 的低频路径更新，不给每条普通 en队列 增加统计 atomic。

## 完成与出队的区别

出队并不等于后端完成。

The 工作线程 may re移交 a record from the 队列 and still be formatting or writing
it. Therefore:

- `dequeue_pos` means slot consumption;
- `async_completed` means 后端 attempt completed;
- `completed_pos` is the condition-variable watermark used by 刷新/wait.

Flush must never equate "队列 is empty" with "output attempt is complete".

## Global 发布

The global logger uses one atomic ticket containing generation + phase so a
reader cannot combine a phase from one generation with another generation.

Publication sequence is conceptually:

```text
construct candidate privately
    |
obtain controller/lifetime publication rights
    |
publish g_logger
    |
release-store RUNNING ticket
```

Readers:

```text
read ticket
    |
admission check
    |
take lifetime read pin
    |
recheck same ticket
    |
borrow g_logger
```

The 生命周期固定, not the atomic 指针/ticket alone, prevents destruction.

## Worker 生命周期

The logger 实例 owns its 工作线程.

Stop protocol:

```text
running = false
    |
wake worker
    |
worker drains until stop predicate
    |
pthread_join()
    |
free worker-owned workspace/queue resources
```

Do not cancel the private 工作线程 as a normal shutdown mechanism.

## Atomic usage rules

Before introducing an atomic field, document:

1. which invariant is represented by the atomic;
2. who writes it;
3. who reads it;
4. the 发布 relation, if any;
5. why the selected memory order is sufficient.

Do not use `memory_order_seq_cst` as a substitute for defining the protocol.

An atomic 指针/counter does not automatically protect the 生命周期 of the
object it names or counts.

## Reference counting

There is currently no generic per-object refcount in production code.

In particular:

- `live_objects` is a process-wide census used for lifecycle/fork gating; it
  does not keep a particular `logger_t` alive and reaching zero does not
  release one;
- `g_lifetime_lock` is a read-side 生命周期固定 for the global logger, not a
  counted reference;
- 工作线程 and 队列 生命周期 is owned by `logger_t` and ends through join.

If independent 所有者s later need to retain an object beyond a lock/pin/所有者
scope, follow `REFCOUNTING.md`; do not build a 生命周期 protocol directly from
raw atomic increment/decrement operations.

## Cancellation and concurrency

Normal Logger/Console calls temporarily disable deferred cancellation around
resource-critical sections and restore the caller state after cleanup.

This does not make arbitrary library code async-cancel-safe. Enabled
`PTHREAD_CANCEL_ASYNCHRONOUS` entry into ordinary APIs remains outside the
supported contract unless explicitly documented otherwise.

## Fork boundary

Raw multi-threaded fork inherits synchronization state. The 子进程 guard rejects
use before touching inherited locks. Scope cleanup and atomics do not make
inherited pthread synchronization objects safe after raw fork.
