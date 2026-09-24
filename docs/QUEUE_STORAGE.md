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
    + 256B inline text
    + optional spill index

预分配 spill pool
    = 最多 1024 × LOGGER_MESSAGE_MAX
```

public API、格式化结果和同步路径仍使用完整 `logger_message_t`。

## inline 与 spill 边界

- 正文长度 < 256B：直接保存在 `inline_text`；
- 正文长度 >= 256B：从 spill pool 取得一个 block；
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

spill pool 用尽时绝不把 long message 截成 255B。

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
- `spill_mu`；
- `wait_mu/wait_cv`。

最终都由 `logger_queue_destroy()` 在 worker join 后释放/销毁。

### 单个 spill block

单个 block 的生命周期：

```text
queue free list
    ->
producer 临时独占
    ->
producer 写正文
    ->
slot release-publish spill index
    ->
consumer acquire slot
    ->
consumer 复制正文到 worker batch
    ->
consumer 归还 free list
```

producer 在 queue full 时会在返回前把 block 归还，因此不会泄漏。

不使用 refcount：block 的 ownership 在 producer -> published slot/consumer -> pool
之间是严格 move/独占关系。

## concurrency

`spill_mu` 只保护 freelist head/next：

```text
lock spill_mu
pop/push one index
unlock spill_mu
```

长正文 memcpy 不在锁内。

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

`spill_mu` 与 `wait_mu/emit_mu/progress_mu` 不允许嵌套。

## source/context snapshot

本轮没有缩小 source/context snapshot：

- module storage 128B；
- file storage 256B；
- function storage 128B；
- request/session/trace 各 64B。

这样 queue 内存优化不会同时改变 source truncation 语义。

slot 复用前会重新清零 source storage，避免上一条 record 的尾部字节被复制到新的
worker batch。

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
