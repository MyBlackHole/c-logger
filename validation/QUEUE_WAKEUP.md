# 队列自节奏唤醒 验证

日期：2026-09-25

本报告验证 PR #14 的 工作线程自节奏唤醒。目标不是改变 队列拓扑，而是消除
“每个成功 入队 都执行一次 等待互斥锁 + 条件变量信号”的 生产者 固定成本，同时保留
进入等待窗口 的 丢失唤醒正确性。

## 对比版本

- 内存基线:
  `23504f104e900419e891b57d7187ca0bcabffdf3`
- throughput/wakeup baseline:
  `03223374f3b99ea0850354aa3d7c345d7f2ddd73`
  - PR #12 merge；
  - 每个成功 入队 都调用 `logger_queue_notify()`；
  - 因此 生产者 cond signal 数等于 入队d。
- 候选版本:
  `276769cf3877875c3802cc956390d53f5c7b72b9`

GitHub Actions benchmark run：`36085045385`。
Ubuntu 24.04.5 / GCC 13.3.0 / 发布 static。
每组合 3 次，每线程 1000 records，取 median。

## Correctness protocol

worker：

```text
lock wait_mu
consumer_waiting = 1
SC fence
recheck queue + running
still empty/running -> pthread_cond_wait
```

生产者：

```text
release-publish slot.seq
SC fence
consumer_waiting == 0
    -> return
consumer_waiting == 1
    -> lock wait_mu
    -> exchange 1 -> 0
    -> winner signal
```

两个 SC fence 是 store-buffering handshake，保证不能同时发生：

```text
worker 看不到新 record
AND
producer 看不到 consumer_waiting
```

shutdown 独立执行 force wake，不依赖 waiting hint。

## Wakeup 结果

| threads | bytes | old wake | new wake | signal reduction | worker waits |
|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 1,000 | 1 | 99.90% | 2 |
| 1 | 256 | 1,000 | 1 | 99.90% | 2 |
| 1 | 1024 | 1,000 | 240 | 76.00% | 241 |
| 1 | 4000 | 1,000 | 2 | 99.80% | 3 |
| 4 | 64 | 4,000 | 1 | 99.98% | 2 |
| 4 | 256 | 4,000 | 1 | 99.98% | 2 |
| 4 | 1024 | 2,913 | 3 | 99.90% | 4 |
| 4 | 4000 | 2,814 | 2 | 99.93% | 3 |
| 16 | 64 | 16,000 | 1 | 99.99% | 2 |
| 16 | 256 | 16,000 | 1 | 99.99% | 2 |
| 16 | 1024 | 3,774 | 2 | 99.95% | 3 |
| 16 | 4000 | 3,443 | 3 | 99.91% | 4 |

over加载 场景的 old wake 使用 baseline 实际 入队d 数，而不是 attempted 数。

结构性结论很明确：

> backlog 持续存在时，生产者 不再为每条 record 做 mutex/condvar notification。
> 通知次数已经接近 worker 真正 idle/sleep 的次数。

1 thread / 1024B 的 worker 频繁追上 生产者，所以实际发生 241 次 sleep、240 次 生产者
wake；这也是预期行为：self-paced 不是“尽量不唤醒”，而是“只在 consumer 确实需要唤醒时
通知”。

## Throughput

无 over加载 场景的同 运行器 median：

| threads | bytes | 生产者 ratio | end-to-end ratio |
|---:|---:|---:|---:|
| 1 | 64 | 1.008x | 0.978x |
| 1 | 256 | 1.036x | 1.012x |
| 1 | 1024 | 1.053x | 1.053x |
| 1 | 4000 | 1.032x | 1.039x |
| 4 | 64 | 1.091x | 1.207x |
| 4 | 256 | 1.110x | 1.031x |
| 16 | 64 | 1.017x | 0.933x |
| 16 | 256 | 0.946x | 1.074x |

共享 运行器 的 throughput 有明显抖动，因此这些 ratio 只用于观察，不作为硬门禁。

可以确认的是：

- SC fence 没有形成明显的系统性 生产者 regression；
- 4-thread short-message 生产者 在本次 run 提升约 9%~11%；
- 部分 1T/16T 组合有几个百分点正负变化，应视为 noise + 调度差异，而不是长期性能承诺；
- 真正稳定且数量级明显的收益是 cond signal 次数下降 76%~99.99%。

## Memory

候选版本 与 PR #12 队列 layout 相同：

```text
slot bytes: 1272
default queue: 13.94 MiB
relative to pre-compact: -63.38%
```

self-paced wakeup 没有扩大 队列/溢出区 固定内存。

新增的 waiting/counter 原子变量s 位于 队列 control object，不按 队列 capacity 放大。

## Regression / Sanitizer

候选版本 已通过：

- native 发布；
- shared ABI inspection；
- native Debug；
- ASan/UBSan；
- TSan；
- 队列 benchmark memory gate。

专项 regression：

- wait-entry publication gap；
- 生产者 slow path 等待同一 `wait_mu`；
- backlog/no-waiter 时 生产者 signal = 0；
- 多 生产者 MPSC；
- force-stop wake；
- 8 生产者 × 700B long-message TSan path。

## 结论

self-paced wakeup 可以保留。

它没有改变：

- 队列 publication；
- bounded memory；
- overflow policy；
- completion/flush；
- 公开 API/ABI；
- worker 所有权/lifecycle。

下一阶段不再继续优化 notification，而应按 Architecture roadmap 进入
**demand-driven 生产者 元数据 capture**，随后再做 consumer-stage profiling。
