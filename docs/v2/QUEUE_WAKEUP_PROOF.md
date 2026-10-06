# Queue sleep/wakeup C11 proof

状态：**#103 第二阶段 production contract**。

本文件证明 queue 的 self-paced sleep/wakeup handshake。它不证明整个 MPSC
queue，也不把 condition variable 当成 payload publication primitive。

## 1. 目的

producer 在 worker 没有准备睡眠时不获取 `wait_mu`、不调用
`pthread_cond_signal()`，保留 self-paced notification；同时必须保证最后一条
record 不会因为 lost wakeup 永久留在 queue 中。

危险结果只有一个：

```text
worker recheck 看不到新 record
AND
producer 看不到 worker 已 arm
AND
worker 随后进入 cond_wait
```

## 2. 为什么删除旧 SC-fence handshake

旧实现使用：

```text
worker:   store waiting=1 -> SC fence -> load slot.seq
producer: store slot.seq   -> SC fence -> load waiting
```

在实际主流架构上它可以表现为经典 Store-Buffering fence idiom，但 #103 的底线
不是“当前 CPU 看起来工作”，而是 **ISO C11/C17 可证明**。

WG14 DR 407 对 C11/C17 的 SC-fence 规则明确保留了这样的警告：SC fence 的
total order 只直接覆盖 fence 本身，混用较弱 ordering 时不能一般化地声称恢复
sequential consistency。后来 C++ P0668 又专门加强了 SC-fence 语义。

因此 c-logger 不把更晚的 C++ 加强规则倒灌成 C11 proof。旧双 fence 从
production protocol 中删除。

## 3. 新协议：单 atomic RMW handoff

`consumer_waiting` 仍只表示 0/1：

```text
0 = worker 未承诺睡眠
1 = worker 在 wait_mu 下已经 arm，准备 recheck/cond_wait
```

但并发阶段对它的访问规则改变为：

> 初始化之后，只要 producer/worker/force-wake 仍可能并发，所有修改都必须是
> acq_rel read-modify-write；禁止加入 plain atomic store 修改该状态。

worker：

```text
lock wait_mu
exchange(waiting, 1, acq_rel)       // arm
recheck queue/running
  work/stop -> exchange(waiting, 0, acq_rel) -> unlock
  empty/run -> pthread_cond_wait
               ...
exchange(waiting, 0, acq_rel)       // disarm
unlock
```

producer：

```text
write payload
slot.seq = published (release)
fetch_or(waiting, 0, acq_rel)       // RMW probe; value itself不变
  old == 0 -> return
  old == 1 -> lock wait_mu
              exchange(waiting, 0, acq_rel)
                old == 1 -> signal
                old == 0 -> someone already resolved the wait state
              unlock
```

shutdown：

```text
running = 0
lock wait_mu
exchange(waiting, 0, acq_rel)
cond_signal                          // unconditional force wake
unlock
```

## 4. C11 基础

本 proof 只依赖 C11/C17 的三个基础性质：

1. 每个 atomic object 有自己的 modification order；
2. atomic read-modify-write 总是读取该 modification order 中紧邻其写入之前的
   最后一个 value；
3. release operation 与读取其值/其 release sequence 值的 acquire operation
   建立 synchronizes-with，进而形成 happens-before。

`atomic_exchange_explicit(..., memory_order_acq_rel)` 和
`atomic_fetch_or_explicit(..., memory_order_acq_rel)` 同时承担 acquire 与
release。

## 5. payload publication 不变

Queue record 的 linearization/publication point 仍然是：

```c
atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
```

consumer 只有：

```c
atomic_load_explicit(&slot->seq, memory_order_acquire) == pos + 1
```

后才读取普通 `slot.record`。

wakeup handoff 不替代 `slot.seq` publication，只保证 worker 在“决定是否睡眠”
这一点不会与 producer 双向互相 miss。

## 6. MPSC reservation gap 边界

MPSC 的 `enqueue_pos` 只分配 position，不发布 payload。因此 producer 可以出现：

```text
P0 reserve position N      // head, 暂未 publish
P1 reserve position N+1
P1 publish N+1
P1 probe waiting
worker arm
worker recheck position N  // 仍然 empty
```

所以“worker acquire 了某个 producer 的 wakeup handoff”**不等于**“当前 dequeue
head 一定已经可消费”。最小 proof 中的 Q 必须理解为：**关闭当前 head gap 的
publication**。

上述 gap 不会造成 lost wake：

1. 如果 head producer P0 在 worker arm **之前** publish + probe，则 P0 的 RMW
   也进入同一 handoff chain，worker arm acquire 之后 recheck 能看到 head
   publication；
2. 如果 P0 在 worker arm **之后** publish，则它必须在 publication 后执行自己的
   RMW probe；worker 尚需睡眠时 waiting=1，因此 P0 进入 signal slow path；
3. 如果中间已有 producer/worker/force-wake resolver 把 waiting 清成 0，则由
   第 7 节的 resolver 规则保证该 wait cycle 已经被解决。

如果某 producer reserve 了 head position 后永久不再 publish，则 queue 本身已经
因为 reservation hole 无法前进；这是 MPSC producer progress/cancellation 问题，
不是 wakeup handshake 可以修复的问题。production logger API 在 runtime scope
内禁用 deferred cancellation，避免在 queue publication 临界路径中被取消。

`queue_wakeup_litmus` 的并发核心测试直接建模 head publisher；另有确定性
reservation-gap case 验证“later slot 先 publish 后，head publisher 仍负责
resolve 已 arm waiter”。

## 6. 最小两线程 proof

定义：

```text
Q = producer release-publish slot.seq
P = producer acq_rel RMW probe waiting (fetch_or 0)

A = worker acq_rel RMW arm waiting=1
R = worker acquire recheck slot.seq
```

且有 program order：

```text
Q sequenced-before P
A sequenced-before R
```

因为 A、P 都是同一个 `consumer_waiting` 上的 RMW，它们在该 atomic 的
modification order 中必有先后。

### Case 1: P 在 A 之前

如果两者之间没有其他 modification，A 必须读取 P 写入的 0。

P 是 release，A 是 acquire，因此：

```text
Q -> P  synchronizes-with  A -> R
```

得到：

```text
Q happens-before R
```

所以 R 对对应 `slot.seq` 的 acquire observation 不能继续观察 Q 之前的
generation；worker 会看到该 record 已发布，不会睡眠。

如果 P 与 A 之间存在其他 producer 的 acq_rel RMW，它们逐个 acquire 前驱、
再 release 给后继，形成传递 handoff chain；结论不变。

### Case 2: A 在 P 之前

没有 intervening clear 时，P 的 RMW 必须读取 A 写入的 1，因此返回
`old == 1`，producer 进入 `wait_mu` slow path。

worker 从 arm 到决定 cond_wait 一直持有同一把 `wait_mu`：

- worker recheck 若已经看到 work/stop，会先 disarm 并 unlock；
- worker 若仍决定睡眠，`pthread_cond_wait()` 原子释放 mutex 并进入等待；
- producer 取得 mutex 后 claim 1->0，并 signal。

所以 producer 不会把 signal 发在 worker “已经承诺睡、但还没进入 wait” 的缝隙里。

## 8. intervening 0 为什么安全

MPSC 下 P 可能在 A 之后，却读到 0；这只能因为 A 与 P 之间已经出现新的
modification。production 中能把 1 变成 0 的路径只有：

1. **producer signal claim**：持 `wait_mu` 的 producer 清 1->0，随后 signal；
2. **worker disarm**：worker 自己 recheck 后决定不睡，或从 wait 返回后退出该
   wait cycle；
3. **force wake**：shutdown 持 `wait_mu` 清状态并无条件 signal。

三种 0 都已经把“这一次 arm 是否还需要通知”解决掉。因此后来 producer probe
读到 0 可以直接 fast-return，不构成 lost wake。

## 9. 为什么所有修改都必须是 RMW

设多个 producer 在 worker arm 前连续 publish：

```text
Q1 -> P1 -> P2 -> P3 -> A
```

P1/P2/P3/A 都是 acq_rel RMW。每个 RMW 读取 modification order 的直接前驱：

```text
P1 release
  -> P2 acquire/release
     -> P3 acquire/release
        -> A acquire
```

因此 publication happens-before 可以沿 chain 传递给 A。

如果中间插入一个 plain store(0)，后续 A 可以只 acquire 那个 store，而不是
producer 的 release chain；proof 会被破坏。所以：

```text
atomic_init(..., 0)      // publish 前允许
plain store while live   // 禁止
acq_rel RMW while live   // 必须
```

litmus harness 在两侧线程都停在 barrier 时使用 relaxed store 重置下一轮，这是
测试 quiescent setup，不是 production 并发协议。

## 10. multi-producer

多个 producer 的 probe 都是 `fetch_or(waiting, 0, acq_rel)`：

- waiting=0 时它们仍形成 RMW modification-order/release-acquire chain；
- worker 后来的 arm acquire 最后一项即可传递看到先前 publications；
- waiting=1 时一个或多个 producer 可能进入 slow path；
- 在 `wait_mu` 下只有第一个 `claim_signal()` 能从 1 清成 0 并 signal；
- 后续 producer claim 读到 0，直接退出。

因此 MPSC 不要求“每个 producer 都 signal”；只要求当前 arm 有一个 resolver。

## 11. condvar / spurious wakeup

`wait_cv` 只是阻塞通知，不是状态。

worker 始终：

```text
lock wait_mu
while (queue empty && running)
    arm
    recheck
    cond_wait
disarm
unlock
```

每次返回都重新检查 predicate，因此 spurious wakeup 安全。

## 12. shutdown / failure

- shutdown 的 `logger_queue_wake_force()` 不依赖 waiting hint，始终 signal；
- `running=0` 后 worker predicate loop 会退出；
- `pthread_cond_wait()` 错误写入 `lifecycle_error` 并转 STOPPING；
- queue storage final free 仍依赖 worker join/quiescence，不依赖
  `consumer_waiting`。

## 13. ABA / wraparound

`consumer_waiting` 不是 generation/identity，只是当前 wait-cycle 的 handoff
state。其正确性来自：

- 单 consumer；
- `wait_mu` 串行化 arm -> recheck -> sleep/disarm；
- 同一 atomic 的 RMW modification order；
- producer resolver 规则。

queue position/generation wraparound 仍由 `slot.seq` 单独处理。

## 14. progress 与成本

整体协议不是 lock-free/wait-free：

- worker 可以阻塞在 condvar；
- producer probe fast path不拿 mutex，但每个成功 enqueue 增加一个 acq_rel RMW；
- 观察到 waiting=1 的 producer 可以阻塞在 `wait_mu`；
- queue full/spill exhausted 仍按 bounded-overload policy 返回。

RMW 会增加 `consumer_waiting` cacheline 的写竞争，因此 #103 必须用既有
queue-benchmark 对比吞吐/notification。若性能不可接受，应在 #104 重新选择
wakeup primitive；不能以未证明的 C11 协议换性能。

## 15. C11 data-race 合法性

- `slot.seq`、`consumer_waiting`、`running` 都是 atomic；
- payload 由 `slot.seq` release/acquire 发布；
- wait-entry 由 `wait_mu` 与 condvar contract 串行化；
- 本协议不存在依赖 `volatile` 或普通共享对象的无同步读写。

## 16. 验证

- `queue_notify_regression`：确定性覆盖 wait-entry gap；
- `queue_wakeup_litmus`：直接竞争 worker arm 与 producer RMW probe，禁止
  “worker misses record && producer misses arm”；
- `queue_mpsc_regression`：8 producer + single consumer；
- TSan：覆盖 notify/MPSC/litmus；
- shutdown force-wake regression；
- queue benchmark：评估每 enqueue 一次 acq_rel RMW 的代价。

测试是 proof evidence，不替代以上 C11 reasoning。

## 17. 规范依据

- ISO/IEC 9899:2018 5.1.2.4 / 7.17.3：atomic modification order、
  release sequence、release/acquire synchronizes-with；
- ISO/IEC 9899:2018 7.17.7：atomic read-modify-write generic operations；
- WG14 DR 407：SC fence 的边界与 C11/C17 修订背景；
- C++ P0668 仅作为“后来标准为何加强 SC fence”的历史证据，不作为本 C11
  correctness proof 的前提。
