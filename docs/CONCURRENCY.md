# Concurrency model

c-logger separates lifetime and synchronization mechanisms that must not be
treated as interchangeable:

```text
atomic ordering -> publication/individual atomic state
lock            -> multi-field invariant serialization
lifetime pin    -> object cannot be released while borrowed
join            -> thread/user lifetime has ended
refcount        -> independent owners count lifetime references
state machine   -> legal lifecycle transitions
```

## MPSC queue publication protocol

The queue is bounded MPSC with per-slot sequence values.

Producer:

```text
reserve enqueue position
    |
write complete compact queue record
    |
release-store slot.seq = published
```

Consumer:

```text
acquire-load slot.seq
    |
if published: read payload
    |
release-store slot.seq = reusable generation
    |
advance dequeue position
```

The release/acquire pair around `slot.seq` publishes the record contents.
Relaxed position counters are reservation/accounting mechanisms and must not be
upgraded/downgraded casually without reviewing the whole protocol.

## Queue long-message spill protocol

queue slot 可内嵌最多 512B 正文。更长消息先从预分配 spill pool
取得一个独占 block，再 reserve/publish queue slot。

publication 顺序为：

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

consumer 通过 acquire-load `slot.seq` 后读取 compact record 与 spill text，
复制到 worker batch，然后在把 slot 标记为 reusable **之前**归还 spill block。

spill block ownership 由固定 atomic bitmap 管理：producer 通过 0->1 CAS claim，
consumer 完成复制后通过 1->0 atomic clear release。这里不再存在共享 spill mutex。

spill pool 暂时耗尽不允许截断 long message。queue push 返回 unavailable，
上层继续使用原有 DROP / SYNC overflow policy。

## Queue self-paced wakeup protocol

MPSC payload publication 与 worker sleep/wakeup 是两个独立协议。

worker 只有在 drain 发现 queue 为空后才进入 wait protocol：

```text
lock wait_mu
  ->
consumer_waiting = 1 (release)
  ->
SC fence
  ->
recheck queue + running
  ->
still empty/running ? cond_wait : do not sleep
```

producer 先完成 `slot.seq` release publication，再执行 SC fence，然后：

```text
consumer_waiting == 0
    -> 直接返回，不碰 wait_mu

consumer_waiting == 1
    -> lock wait_mu
    -> exchange waiting 1 -> 0
    -> 成功者 signal 一次
```

两个 SC fence 构成 store-buffering handshake，因此不能同时出现
“worker 看不到新 record”和“producer 看不到 waiting”。

所以 publish 落在任意窗口时都不会丢 wakeup：

- producer 没看到 waiting 时，worker 的二次 recheck 必须看到已 publish record；
- worker 的二次 recheck 没看到 record 时，producer 必须观察到 waiting 并在
  `pthread_cond_wait()` 原子释放 mutex 后取得 `wait_mu` signal。

多个 producer 同时观察到 waiting=1 时，只有一个能通过 exchange 消费通知 ownership。

stop/shutdown 不依赖这个 hint，而是设置 `running=false` 后执行独立 force wake。

private diagnostics：

- `wait_count`：实际进入 cond_wait 的次数；
- `producer_wake_signals`：producer 真正发送的 signal 次数；
- `force_wake_signals`：shutdown/stop 强制 signal 次数。

这些计数只在真实 wait/signal 的低频路径更新，不给每条普通 enqueue 增加统计 atomic。

## Completion versus dequeue

Dequeue is not backend completion.

The worker may remove a record from the queue and still be formatting or writing
it. Therefore:

- `dequeue_pos` means slot consumption;
- `async_completed` means backend attempt completed;
- `completed_pos` is the condition-variable watermark used by flush/wait.

Flush must never equate "queue is empty" with "output attempt is complete".

## Global publication

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

The lifetime pin, not the atomic pointer/ticket alone, prevents destruction.

## Worker lifetime

The logger instance owns its worker.

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

Do not cancel the private worker as a normal shutdown mechanism.

## Atomic usage rules

Before introducing an atomic field, document:

1. which invariant is represented by the atomic;
2. who writes it;
3. who reads it;
4. the publication relation, if any;
5. why the selected memory order is sufficient.

Do not use `memory_order_seq_cst` as a substitute for defining the protocol.

An atomic pointer/counter does not automatically protect the lifetime of the
object it names or counts.

## Reference counting

There is currently no generic per-object refcount in production code.

In particular:

- `live_objects` is a process-wide census used for lifecycle/fork gating; it
  does not keep a particular `logger_t` alive and reaching zero does not
  release one;
- `g_lifetime_lock` is a read-side lifetime pin for the global logger, not a
  counted reference;
- worker and queue lifetime is owned by `logger_t` and ends through join.

If independent owners later need to retain an object beyond a lock/pin/owner
scope, follow `REFCOUNTING.md`; do not build a lifetime protocol directly from
raw atomic increment/decrement operations.

## Cancellation and concurrency

Normal Logger/Console calls temporarily disable deferred cancellation around
resource-critical sections and restore the caller state after cleanup.

This does not make arbitrary library code async-cancel-safe. Enabled
`PTHREAD_CANCEL_ASYNCHRONOUS` entry into ordinary APIs remains outside the
supported contract unless explicitly documented otherwise.

## Fork boundary

Raw multi-threaded fork inherits synchronization state. The child guard rejects
use before touching inherited locks. Scope cleanup and atomics do not make
inherited pthread synchronization objects safe after raw fork.
