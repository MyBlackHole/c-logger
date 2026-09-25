# Queue compact storage 与 long-message spill pool

## 目标

旧 queue slot 直接内嵌完整 `logger_message_t`，其中仅正文就包含
`text[4096]`。queue capacity 较大时，即使绝大多数日志只有几十到几百字节，也会为每个
slot 固定支付 4KiB 正文空间。

本轮把 queue storage 拆成：

```text
compact slot
    = metadata
    + context snapshot
    + source snapshot
    + 512B inline text
    + optional spill index

预分配 spill pool
    = 最多 1024 × LOGGER_MESSAGE_MAX
```

public API、格式化结果和同步路径仍使用完整 `logger_message_t`。

## inline 与 spill 边界

- 正文长度 <= 512B：直接保存在 `inline_text`；
- 正文长度 > 512B：从 spill pool 取得一个 block；
- 每个 spill block 保留完整 `LOGGER_MESSAGE_MAX` 空间；
- queue capacity < 1024 时，spill block 数量等于 queue capacity；
- queue capacity >= 1024 时，spill block 数量固定上限为 1024。

因此，大 queue 的 long-message capacity 与 slot capacity 是刻意解耦的。

例如默认 queue capacity 8192：

```text
short message capacity = 8192 slots
long message queued capacity <= 1024 spill blocks
```

这是内存上界换取的明确设计，不应被描述成“所有消息都拥有 8192 的有效排队容量”。

## spill exhaustion

spill pool 用尽时绝不静默截断 long message。

`logger_queue_push()` 返回 unavailable，上层继续执行已有 level overflow policy：

```text
TRACE/DEBUG/INFO/WARN default -> DROP
ERROR/FATAL default          -> SYNC fallback
```

因此：

- 不增加 silent truncation；
- 不在 hot path 临时 malloc；
- long-message burst 超出预分配上限时行为可预测；
- benchmark 会单独报告 `queue_spill_exhaustions`。

## ownership

### queue lifetime

`logger_queue_t` 结构性拥有：

- compact `slots`；
- `spills`；
- lock-free `spill_used[]` bitmap；
- `consumer_waiting + wait_mu/wait_cv` self-paced wakeup state。

最终都由 `logger_queue_destroy()` 在 worker join 后释放/销毁。

### 单个 spill block

单个 block 的生命周期：

```text
queue free bitmap
    ->
producer CAS claim 后临时独占
    ->
producer 写正文
    ->
slot release-publish spill index
    ->
consumer acquire slot
    ->
consumer 复制正文到 worker batch
    ->
consumer atomic clear 归还 bitmap
```

producer 在 queue full 时会在返回前把 block 归还，因此不会泄漏。

不使用 refcount：block 的 ownership 在 producer -> published slot/consumer -> pool
之间是严格 move/独占关系。

## concurrency

`spill_used[]` 是固定大小 atomic bitmap，不使用共享 mutex/freelist。

producer 扫描 bitmap word，通过 0->1 CAS 独占 block；consumer 完成正文复制后，
通过 atomic clear 把 bit 从 1->0。每个线程从自己的 TLS probe 起点开始扫描，避免所有
producer 从同一个 word 开始竞争。

正文 memcpy 发生在 block claim 成功之后，不需要持锁。

slot publication 仍沿用原 MPSC release/acquire：

```text
producer 写 spill text + compact record
    ->
release-store slot.seq

consumer acquire-load slot.seq
    ->
读取 compact record / spill text
```

consumer 必须先完成正文复制并归还 spill block，再把 slot.seq 标成 reusable。

`spill_used[]` 不属于 mutex hierarchy。它只承担 block ownership 状态；
slot publication 仍由 `slot.seq` 的 release/acquire 保证。

## source/context snapshot

本轮没有缩小 source/context snapshot：

- module storage 128B；
- file storage 256B；
- function storage 128B；
- request/session/trace 各 64B。

这样 queue 内存优化不会同时改变 source truncation 语义。

compact record 额外保存 module/file/function 的实际长度。producer 只覆盖当前字符串，
consumer 只复制 `len+1`，因此不再为每条日志固定 memset/memcpy 整个 512B source 尾部。

## worker workspace

worker batch 仍使用完整 `logger_message_t`，但 capacity 最大为 256。

这是有意保留的第二阶段结构：

- queue 承担大规模 pending record storage，因此优先 compact；
- worker batch 只保存一个有限批次，继续使用完整 message 可保持 format/backend 路径简单；
- 后续如果 benchmark 证明 workspace 仍值得优化，可单独设计，不与 queue publication
  同时变化。

## benchmark

`bench_matrix` 现在报告：

- `queue_slot_bytes`
- `queue_slot_storage_bytes`
- `queue_spill_capacity`
- `queue_spill_storage_bytes`
- `queue_spill_exhaustions`
- `queue_storage_bytes`

`queue_storage_bytes` 只统计 queue 的预分配 slot + spill storage，不包含
`logger_t`、pthread object 和 worker workspace。

benchmark matrix 继续覆盖 64 / 256 / 1024 / 4000B 消息，长消息压力下必须同时观察
throughput、drop/sync fallback 与 spill exhaustion，不能只比较 logs/sec。

## hot-path 优化

首次 compact benchmark 暴露出两个不同热点：

1. 256B 正文过早进入 spill path，4/16 producer 出现明显 spill exhaustion；
2. 4KiB 单 producer 即使没有 spill exhaustion 也明显变慢，说明不能只归因于 pool 容量。

因此当前实现做三项针对性优化：

- inline 最大正文从 255B 提升到 **512B**；
- spill freelist mutex 改为 lock-free atomic bitmap；
- `logger_message_t.text_len` 直接保存 `vsnprintf()` 已知的实际写入长度，
  async enqueue 不再重新扫描最多 4095B 正文。

source snapshot 同时保存实际长度，减少固定尾部内存流量。

这些优化不扩大 1024 block spill 上限，先解决 hot path，再根据同 runner benchmark
判断是否真的需要调整 capacity。


## before/after benchmark 验证

benchmark workflow 现在同时构建三个版本：

```text
memory baseline:
23504f104e900419e891b57d7187ca0bcabffdf3
    = pre-compact，仅用于 >=50% 内存门禁

throughput tuning baseline:
62dbe16393141899b8026c5c7f81e2415d87f634
    = 256B inline + spill_mu 的上一版 compact

candidate:
当前 commit
```

三者在同一个 GitHub Actions runner、同一编译器和同一 Release 配置中构建。

对比矩阵：

```text
threads = 1 / 4 / 16
message = 64 / 256 / 1024 / 4000 B
每线程 1000 records
每个组合重复 3 次，取 median
```

### 确定性门禁

吞吐受共享 runner 调度、CPU frequency、虚拟化噪声影响，因此不把 logs/sec ratio
作为 CI pass/fail 条件。

硬门禁继续以 pre-compact layout 为内存基线：

```text
default queue capacity = 8192
candidate queue storage 相对 pre-compact 至少下降 50%
```

### hotspot 对比

吞吐 ratio 则相对“上一版 compact”计算，直接衡量本轮 512B inline、bitmap allocator、
text_len/source-length 优化的收益。

只有 tuning baseline/candidate 都没有 drop、sync fallback、spill exhaustion 时才计算
ratio；否则标记为 `overload`，避免把少处理日志误解释成性能提升。

workflow 输出完整 raw samples、Markdown summary 和 Actions artifact。实测结论写入
`validation/QUEUE_HOTPATH.md`。


## self-paced worker wakeup

普通 enqueue 不再无条件执行 `pthread_mutex_lock + pthread_cond_signal`。

worker 在 queue 空时：

```text
wait_mu
  -> publish consumer_waiting=1
  -> SC fence
  -> recheck queue/running
  -> cond_wait
```

producer 在 record release-publish 后先执行 SC fence，再读取 `consumer_waiting`：

- 0：worker 正在运行或尚未准备睡眠，直接返回；
- 1：进入 `wait_mu` slow path，只有一个 producer exchange 成功并 signal。

shutdown/stop 使用独立 force wake，不依赖 hint。

benchmark 私有字段同时报告 `queue_wait_count`、`queue_producer_wake_signals` 和
`queue_force_wake_signals`。上一版实现每个成功 enqueue 都 signal，因此 tuning
baseline 的 producer signal 数等于 enqueued；candidate 可以直接计算 signal reduction。


SC fence 是 correctness protocol 的一部分，不是可随意删除的性能细节。它防止 worker 和
producer 在 store-buffering 交错下同时读到对方的旧状态；如果未来要替换该 barrier，
必须重新证明 lost-wakeup 不可能，并用 wait-entry regression + TSan 验证。
