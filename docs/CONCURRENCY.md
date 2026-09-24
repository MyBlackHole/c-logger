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
write complete logger_message_t payload
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
producer 从 spill freelist 取得 block（仅长消息）
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

## Queue wakeup protocol

The MPSC atomic protocol and the worker sleep protocol are separate.

`q.wait_mu + q.wait_cv` ensure that a producer cannot signal in the critical
window between the worker observing an empty queue and atomically sleeping.
Removing the mutex around the signal/wait path requires a separately proven
wakeup protocol.

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
