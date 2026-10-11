# c-logger 2.0 C11 内存模型

状态：**#103 设计基线，适用于当前 Audit-free ABI 2**。

当前设计入口见 [README.md](README.md)；已退役 Audit publication/failure-policy 不再是本版本协议或测试证据。

2.0 的用户态并发首先服从 ISO C11 data-race rules。Linux 内核中的 publication、RCU、seqcount、barrier 等思想可以借鉴，但不能直接复制 kernel-only memory-model 假设。

## 1. 基本规则

- `volatile` 不是同步、不是 atomic、不是 memory barrier。
- 普通非 atomic 对象存在并发读写且没有锁/同步时，即使最终会 retry，也属于 C11 data race。
- `memory_order_relaxed` 只提供 atomicity 和该 atomic object 的 modification-order/coherence，不发布依赖 payload。
- release/acquire 只有建立在明确的 synchronizes-with / happens-before 边上时才用于 payload publication。
- mutex/rwlock 的 lock/unlock 是同步边；由锁保护的普通对象不需要再人为变成 atomic。
- atomic state 不能替代对象 lifetime；先证明指针仍然可解引用，再谈 atomic load/CAS。
- `seq_cst` fence 不是“更安全”的装饰；必须有明确的 lost-wakeup/ordering proof 和 litmus test 才允许使用。

## 2. helper 设计原则

当前不引入泛化的 `load_acquire(x)` / `store_release(x)` 宏来隐藏 C11 atomic API。

原因：

- 不同 atomic object 的 memory order 目的不同；
- 泛型包装容易让 reviewer 看不到 publication point；
- 很多 relaxed operation 是有意的 reservation/metric，而不是缺少 acquire/release。

优先采用**协议语义 helper**，例如未来可以考虑：

```text
queue_slot_publish()
queue_slot_observe()
queue_slot_release_for_reuse()
logger_stop_publish()
global_ticket_publish()
```

这些 helper 必须在注释中写明对应 happens-before 关系。

## 3. Queue：slot publication

当前 queue 使用 MPSC reservation + 单 consumer slot generation。

### producer

```text
enqueue_pos relaxed CAS
    -> 独占逻辑 position
    -> 写普通 slot.record payload
    -> slot.seq release store(pos + 1)
```

`enqueue_pos` 只负责 reservation 唯一性，不发布 payload。

真正的 record publication point 是：

```c
atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
```

### consumer

consumer 只有在：

```c
atomic_load_explicit(&slot->seq, memory_order_acquire) == pos + 1
```

之后才读取普通 `slot.record`。

因此：

```text
producer payload writes
    -> slot.seq release
    -> slot.seq acquire
    -> consumer payload reads
```

构成 C11 happens-before，普通 payload 不发生 data race。

## 4. Queue：slot reuse

consumer 将完整 record 复制到 worker batch，并归还该 record 的 spill 后，即发布 slot 可复用；此时尚未执行 backend 输出：

```c
atomic_store_explicit(&slot->seq, pos + q->cap, memory_order_release);
```

下一轮 producer 在覆写 slot 前 acquire-load 对应 `slot.seq`。

因此旧 consumer 的 payload read 与新 producer 的 overwrite 通过 generation atomic 建立顺序。

不得把 `enqueue_pos/dequeue_pos` 当成 slot payload publication primitive。

## 5. Spill ownership

`spill_used[]` 是 ownership bitmap，不是正文 publication channel。

block 取得：

```text
producer CAS 0 -> 1 (acq_rel)
    -> 当前 producer 独占 block
    -> 写 spill text
    -> 通过 slot.seq release 发布记录
```

consumer 通过 slot.seq acquire 后才读 spill text。

consumer 将 spill 正文复制到独立 worker batch 后，在 slot 可复用之前归还 block：

```text
spill_used release clear
    -> 下一 producer acquire CAS
    -> 允许覆盖该 block
```

因此 spill 的两个关键关系是：

1. 内容 publication 由 `slot.seq` 完成；
2. block reuse ordering 由 `spill_used` release/acquire 完成。

## 6. Queue sleep/wakeup

#103 第二阶段没有继续依赖旧的“双 SC fence + 较弱 atomic”推理。严格按
C11/C17 审查后，queue wakeup 改成**同一个 atomic 上的 acq_rel RMW handoff**：

```text
worker:                              producer:
exchange(waiting, 1, acq_rel)        publish slot.seq (release)
recheck slot.seq (acquire)           fetch_or(waiting, 0, acq_rel)
                                      |
                                      +-- saw 1 -> wait_mu 下 clear + signal
```

核心不是“RMW 更强”，而是 `consumer_waiting` 自己有唯一 modification order，
且每个 RMW 都读取该 order 中紧邻自己的前一个 modification。所有并发的
post-init write 都通过 acq_rel RMW helper：

- `logger_queue_wait_arm_recheck()`；
- `logger_queue_wait_probe_after_publish()`；
- `logger_queue_wait_claim_signal()`；
- `logger_queue_wait_disarm()`。

因此只有两类情况：

1. producer probe 在 worker arm 之前：arm acquire 通过 RMW chain 取得 producer
   release 之前的 `slot.seq` publication，worker recheck 不能继续把该 record
   当作未发布；
2. worker arm 在 producer probe 之前：probe 观察到 waiting=1 并进入
   `wait_mu + cond_signal` slow path；如果中间已经有人把 1 清为 0，那么该
   modification 本身必须来自已证明安全的 signal/disarm/force-wake 路径。

这里的第 1 条对 queue-empty 判定应理解为“**当前 dequeue head 的 producer**
publication 已进入 handoff chain”。若后续 position 先 publish、head position
仍有 reservation gap，worker 仍可看到 empty；但真正关闭 head gap 的 producer
在 publish 后也会执行 probe，并在 worker 已 arm 时负责 signal。该边界在正式
proof 与 litmus 的 reservation-gap case 中单独验证。

这里不再使用 `atomic_thread_fence(memory_order_seq_cst)`。原因不是实现机器上
SC fence 一定错误，而是 c-logger 的 correctness contract 是 ISO C11/C17；
WG14 对 SC fence 的规则明确提醒：较弱 atomic 加 SC fence 不能一般化地恢复
sequential consistency。项目不以更晚的 C++ memory-model 加强语义替 C11 作证。

正式 proof、MPSC RMW-chain、condvar wait-entry、shutdown 和验证边界见：

- `docs/v2/QUEUE_WAKEUP_PROOF.md`；
- `tests/regression/test_queue_wakeup_litmus.c`。

litmus/stress/TSan 是 proof evidence，不替代 C11 proof。

## 7. Worker stop state

`running` 是 worker stop/admission state，不是 `logger_t` lifetime pin。

```text
owner release-store running=0
    -> wake worker
    -> worker acquire-load running
    -> worker exits
    -> pthread_join
    -> final memory reclamation allowed
```

真正证明 worker 不再访问 parent memory 的机制是 `pthread_join()`，不是 `running=0`。

## 8. Logger state

`logger_t.state` 只表示 API admission/state machine。

- RUNNING -> STOPPING/STOPPED 使用 release store；
- API 通过 acquire load 判断是否接受操作；
- 它不保护 caller 手中的裸 `logger_t *` lifetime。

外部 caller lifetime 仍由 #102 ownership contract 保证。

## 9. completion counters

`async_completed/sync_completed` 等完成计数用于观察进度。

`completed_pos` 是 `progress_mu` 保护的 condition predicate，`progress_cv` 必须与该 mutex 配对。

规则：

- condition wait 必须 `while (predicate not satisfied) pthread_cond_wait(...)`；
- 不能把一次 signal/broadcast 当成 completion 本身；
- queue dequeue 只表示 consumer 已复制记录并归还 slot/spill，不等于 backend 输出完成；
- worker 在 `emit_batch()` 返回后推进 `async_completed`，同步协议正常时再持 `progress_mu` 推进 `completed_pos`；
- backend I/O 失败也会推进完成进度并记录错误；完成不是成功或持久化证明。

当前 flush watermark 是 predicate-based progress，不等同于 Linux one-shot `completion`。

因此 #103 暂不为了名称一致强行引入 generic completion primitive。

## 10. Global publication / lifetime

`g_ticket` 是 generation + phase admission token。

`g_logger` 是普通 pointer，真正由 `g_lifetime_lock` 保护。

Global init 的 STARTING/RUNNING 及 shutdown 的 STOPPING/STOPPED ticket CAS
统一使用 success=`memory_order_acq_rel`、failure=`memory_order_acquire`。这样保留成功路径的
release publication 与失败路径的 acquire 观察，不依赖历史 C11 “failure 不强于 success”
对 release/acquire 组合的解释歧义。这是契约/可移植性修复，不是已复现 UB 的结论。
`g_ticket` 仍只负责 generation/admission，不提供 `g_logger` 的 lifetime pin；
atomic-order 静态检查不能替代 happens-before 与 lifetime 证明。

publish：

```text
take lifetime write lock
    -> g_logger = candidate
    -> publish RUNNING ticket
    -> unlock lifetime
```

reader：

```text
read ticket
    -> lifetime read lock
    -> recheck ticket
    -> read g_logger while lock held
```

shutdown：

```text
publish STOPPING ticket
    -> lifetime write lock drains readers
    -> g_logger = NULL
    -> unlock
    -> destroy old logger
```

重要结论：

> `g_ticket` 不是 `g_logger` 的 reclamation primitive；rwlock 才是当前 lifetime pin。

不能把该模型直接替换成 `atomic g_logger + refcount_inc_not_zero()`，因为 caller 在 CAS refcount 前仍需要证明 pointer/refcount storage 没被 free。

## 11. Console config

`g_config` 把 verbosity + color 放在一个 atomic snapshot 中。

操作只要求拿到某个完整配置快照，不依赖其他 payload publication，因此 relaxed load/store/CAS 是合理的。

如果未来配置拆成多个字段，则必须重新设计 coherent snapshot，不能用多个 relaxed atomic 假装事务。

## 12. Process atomics

`owner_pid/forked_child/live_objects` 属于 process guard，不是 object lifetime publication。

- `live_objects` 是 census；
- `owner_pid` 是 process identity；
- `forked_child` 是 inherited-runtime rejection bit。

它们不能被当作 Logger pointer reclamation primitive。

## 13. Metrics atomics

纯统计计数默认使用 relaxed。

只有当某 counter 同时承担“事件完成/依赖 payload 可见性”的协议职责时，才允许提升到 release/acquire，并必须在本文登记。

不要因为 diagnostics 读取用了 acquire 就推断其他普通字段随之安全；普通字段仍需要各自的 lifetime/lock contract。

## 14. seqcount 结论

当前不引入 seqcount。

原因：

- C11 下 reader 无锁读取 writer 正在修改的普通 payload 会构成 data race，即使 seq 变化后 retry；
- 现有 metrics 大多已经是独立 atomics；
- backend mutable state 已由 mutex 保护；
- 当前没有已证明的 read-mostly coherent snapshot 热点值得承担复杂度。

以后如需 seqcount-like 方案，必须先给出 C11 合法的数据表示，不能照搬 kernel seqcount 对普通 payload 的假设。

## 15. RCU/SRCU 结论

当前不引入 RCU/SRCU。

Global 可能是未来候选，但在 #104 决定 Global facade/object model 前，现有 rwlock lifetime proof 更简单。

任何未来 RCU 方案必须同时解决：

- reader registration/grace period；
- reclamation；
- fork child；
- cancellation；
- DSO dlclose；
- pointer publication；
- benchmark 证明 rwlock 是真实瓶颈。

## 16. Review checklist

任何新的 lockless/atomic protocol 必须回答：

1. 哪些内存位置是 atomic，哪些是普通 payload？
2. 谁写 payload？
3. linearization point 是什么？
4. publication atomic 是什么？
5. release 在哪里？
6. acquire 在哪里？
7. 哪条边形成 happens-before？
8. reclamation 由什么保证？
9. ABA 是否可能？
10. progress guarantee 是 blocking/lock-free/wait-free 中哪一种？
11. failure/retry 是否会重复 side effect？
12. 有什么 litmus/TSan/fault-injection evidence？
