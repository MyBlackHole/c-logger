# 队列热路径 优化验证

日期：2026-09-24

本轮针对首次 紧凑队列 benchmark 暴露出的热点做定向优化，并使用同一 运行器 的
三版本对比验证。

## 对比版本

- 内存基线：`23504f104e900419e891b57d7187ca0bcabffdf3`
  - 紧凑化前，槽位 内嵌完整 `logger_message_t`
- 吞吐调优基线：`62dbe16393141899b8026c5c7f81e2415d87f634`
  - 256B 内联 + `spill_mu` 空闲链表
- 候选版本：PR #12
  - 512B 内联
  - lock-free 溢出区 bitmap
  - 生产 `text_len`
  - source-length copy

GitHub Actions run：`36015294836`，Ubuntu 24.04.5 / GCC 13.3.0 / 发布 static。
每个组合重复 3 次，每线程 1000 records，取 median。

## 本轮实际处理的热点

### 1. 256B 边界过早进入 溢出区

上一版正文长度达到 256B 就使用 溢出区。实际 benchmark 中：

- 4 生产者 / 256B：出现 溢出区 exhaustion/drop；
- 16 生产者 / 256B：出现大量 溢出区 exhaustion/drop。

本轮把“最大 内联 正文”提高到 512B，512B 本身仍然 内联。

### 2. long-message 二次 O(n) 扫描

`vsnprintf()` 已经返回实际写入长度，但上一版 `logger_queue_push()` 又扫描
`text[]` 查找 NUL。

4KiB 日志等价于每次 入队 多扫描接近 4KiB。

本轮内部 `logger_message_t` 保存实际 `text_len`；生产 路径直接使用
`vsnprintf()` 已有结果。只有测试/手工构造 message 未提供长度时才 fallback 扫描。

### 3. 溢出区 空闲链表 的共享 mutex

上一版每条 long message：

```text
producer -> spill_mu -> pop
consumer -> spill_mu -> push
```

所有 long-message 生产者 与单 consumer 竞争同一把 mutex。

本轮改成 1024-bit 原子变量 所有权 bitmap：

```text
producer: 0 -> 1 CAS claim
consumer: 1 -> 0 atomic clear release
```

生产者 使用 TLS probe 起点，避免所有线程从同一 bitmap word 开始。

### 4. source snapshot 固定尾部复制

上一版 槽位 复用时固定清零/复制 module/file/function 全部 512B storage。

本轮 compact record 保存三个 source string 的实际长度，只复制有效 `len + 1`。

## 内存结果

默认 队列 capacity = 8192。

| 指标 | 紧凑化前 | 上一版 compact | 候选版本 |
|---|---:|---:|---:|
| 槽位 bytes | 4872 B | 1016 B | 1272 B |
| 默认 队列 总预分配 | 38.06 MiB | 11.94 MiB | 13.94 MiB |
| 候选版本 溢出区 | - | 1024 × 4100 B | 1024 × 4096 B |

候选版本 相对 紧凑化前 下降 **63.38%**，50% memory gate 通过。

512B 内联 让固定内存相对上一版 compact 增加 **16.72%**，但换来了 256B 档完全绕开
溢出区 path。

## 吞吐与 over加载

| threads | bytes | base prod/s | new prod/s | prod ratio | base e2e/s | new e2e/s | e2e ratio | base 溢出区 | new 溢出区 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 390,802 | 396,795 | 1.015x | 390,666 | 396,633 | 1.015x | 0 | 0 |
| 1 | 256 | 325,338 | 396,200 | 1.218x | 325,220 | 396,037 | 1.218x | 0 | 0 |
| 1 | 1024 | 292,344 | 315,693 | 1.080x | 292,254 | 315,598 | 1.080x | 0 | 0 |
| 1 | 4000 | 188,465 | 280,201 | 1.487x | 188,431 | 280,119 | 1.487x | 0 | 0 |
| 4 | 64 | 636,939 | 919,712 | 1.444x | 636,838 | 919,551 | 1.444x | 0 | 0 |
| 4 | 256 | 739,846 | 746,485 | over加载 | 523,537 | 746,374 | over加载 | 331 | **0** |
| 4 | 1024 | 430,338 | 789,345 | over加载 | 430,314 | 416,495 | over加载 | 148 | 1,771 |
| 4 | 4000 | 413,352 | 611,219 | over加载 | 404,894 | 349,906 | over加载 | 475 | 1,586 |
| 16 | 64 | 1,091,637 | 1,200,432 | 1.100x | 1,038,125 | 1,000,821 | 0.964x | 0 | 0 |
| 16 | 256 | 1,183,130 | 1,174,478 | over加载 | 514,256 | 1,048,825 | over加载 | 9,014 | **0** |
| 16 | 1024 | 934,940 | 1,256,042 | over加载 | 374,074 | 401,513 | over加载 | 10,148 | 10,935 |
| 16 | 4000 | 580,052 | 1,135,464 | over加载 | 229,816 | 451,326 | over加载 | 9,642 | 9,640 |

出现任一 drop/fallback/溢出区 exhaustion 时不计算正式 ratio，表中的 over加载 行只保留原始
数据用于定位瓶颈。

## 热点结论

### 生产者 固定成本已经明显下降

最有代表性的无 over加载 场景：

- 1 thread / 4KiB：生产者 +48.7%，end-to-end +48.7%；
- 1 thread / 256B：+21.8%；
- 1 thread / 1KiB：+8.0%；
- 4 threads / 64B：+44.4%。

因此上一版 long-message 慢并不只是“溢出区 pool 小”，`spill_mu` 与二次 text scan
确实是实际 hot path。

### 256B 内联 问题已解决

4/16 生产者 的 256B 候选版本 都是：

```text
spill exhaustion = 0
drop = 0
```

上一版分别出现 331 / 9014 次 溢出区 exhaustion（本次同 运行器 tuning baseline 数据）。

说明 512B 内联 是有效的容量/性能折中。

### 下一瓶颈是 long-message burst 背压

1KiB/4KiB 在高 生产者 数下仍会耗尽 1024 溢出区 blocks。

值得注意的是 候选版本 生产者 更快，例如：

- 4 threads / 1KiB：430k -> 789k 生产者/s；
- 16 threads / 4KiB：580k -> 1.136M 生产者/s。

生产者 加速后，单 consumer/backend 更容易在 burst 中落后，所以固定 1024 溢出区
capacity 更快暴露出来。这是 **背压/capacity**，不再是原先共享 mutex 热点。

## 为什么本轮不直接改成 2048 溢出区

在当前 1272B 槽位 下，如果 溢出区 从 1024 提升到 2048：

```text
slots: 1272 * 8192
spill: 4096 * 2048
total: 约 17.94 MiB
```

相对 紧凑化前 38.06 MiB 只剩约 **52.9%** 内存下降，已经非常接近 50% 门禁。

这会用额外约 4MiB 固定内存换取更大的有限 burst buffer，却不会提高 single consumer 的
持续处理速率。对持续 over加载，pool 最终仍会耗尽。

因此本轮保留 1024 block；下一步应先测量/优化 consumer/背压，再决定是否需要
可配置的 burst capacity，而不是用固定内存把瓶颈向后推。

## 正确性验证

runtime 候选版本 已通过：

- native 发布 + shared ABI inspection；
- native Debug；
- ASan/UBSan；
- TSan。

TSan 中的 MPSC 回归现在实际使用 8 生产者 × 700B long messages，共 24000 records，
覆盖 原子变量 bitmap claim/release、溢出区 publication、block reuse 和 队列 rollover。

结论：本轮 bitmap 所有权 协议未发现 data race，公开 API/ABI 无变化。
