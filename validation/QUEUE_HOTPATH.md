# Queue hot-path 优化验证

日期：2026-09-24

本轮针对首次 compact queue benchmark 暴露出的热点做定向优化，并使用同一 runner 的
三版本对比验证。

## 对比版本

- 内存基线：`23504f104e900419e891b57d7187ca0bcabffdf3`
  - pre-compact，slot 内嵌完整 `logger_message_t`
- 吞吐调优基线：`62dbe16393141899b8026c5c7f81e2415d87f634`
  - 256B inline + `spill_mu` freelist
- candidate：PR #12
  - 512B inline
  - lock-free spill bitmap
  - production `text_len`
  - source-length copy

GitHub Actions run：`36015294836`，Ubuntu 24.04.5 / GCC 13.3.0 / Release static。
每个组合重复 3 次，每线程 1000 records，取 median。

## 本轮实际处理的热点

### 1. 256B 边界过早进入 spill

上一版正文长度达到 256B 就使用 spill。实际 benchmark 中：

- 4 producer / 256B：出现 spill exhaustion/drop；
- 16 producer / 256B：出现大量 spill exhaustion/drop。

本轮把“最大 inline 正文”提高到 512B，512B 本身仍然 inline。

### 2. long-message 二次 O(n) 扫描

`vsnprintf()` 已经返回实际写入长度，但上一版 `logger_queue_push()` 又扫描
`text[]` 查找 NUL。

4KiB 日志等价于每次 enqueue 多扫描接近 4KiB。

本轮内部 `logger_message_t` 保存实际 `text_len`；production 路径直接使用
`vsnprintf()` 已有结果。只有测试/手工构造 message 未提供长度时才 fallback 扫描。

### 3. spill freelist 的共享 mutex

上一版每条 long message：

```text
producer -> spill_mu -> pop
consumer -> spill_mu -> push
```

所有 long-message producer 与单 consumer 竞争同一把 mutex。

本轮改成 1024-bit atomic ownership bitmap：

```text
producer: 0 -> 1 CAS claim
consumer: 1 -> 0 atomic clear release
```

producer 使用 TLS probe 起点，避免所有线程从同一 bitmap word 开始。

### 4. source snapshot 固定尾部复制

上一版 slot 复用时固定清零/复制 module/file/function 全部 512B storage。

本轮 compact record 保存三个 source string 的实际长度，只复制有效 `len + 1`。

## 内存结果

默认 queue capacity = 8192。

| 指标 | pre-compact | 上一版 compact | candidate |
|---|---:|---:|---:|
| slot bytes | 4872 B | 1016 B | 1272 B |
| 默认 queue 总预分配 | 38.06 MiB | 11.94 MiB | 13.94 MiB |
| candidate spill | - | 1024 × 4100 B | 1024 × 4096 B |

candidate 相对 pre-compact 下降 **63.38%**，50% memory gate 通过。

512B inline 让固定内存相对上一版 compact 增加 **16.72%**，但换来了 256B 档完全绕开
spill path。

## 吞吐与 overload

| threads | bytes | base prod/s | new prod/s | prod ratio | base e2e/s | new e2e/s | e2e ratio | base spill | new spill |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 390,802 | 396,795 | 1.015x | 390,666 | 396,633 | 1.015x | 0 | 0 |
| 1 | 256 | 325,338 | 396,200 | 1.218x | 325,220 | 396,037 | 1.218x | 0 | 0 |
| 1 | 1024 | 292,344 | 315,693 | 1.080x | 292,254 | 315,598 | 1.080x | 0 | 0 |
| 1 | 4000 | 188,465 | 280,201 | 1.487x | 188,431 | 280,119 | 1.487x | 0 | 0 |
| 4 | 64 | 636,939 | 919,712 | 1.444x | 636,838 | 919,551 | 1.444x | 0 | 0 |
| 4 | 256 | 739,846 | 746,485 | overload | 523,537 | 746,374 | overload | 331 | **0** |
| 4 | 1024 | 430,338 | 789,345 | overload | 430,314 | 416,495 | overload | 148 | 1,771 |
| 4 | 4000 | 413,352 | 611,219 | overload | 404,894 | 349,906 | overload | 475 | 1,586 |
| 16 | 64 | 1,091,637 | 1,200,432 | 1.100x | 1,038,125 | 1,000,821 | 0.964x | 0 | 0 |
| 16 | 256 | 1,183,130 | 1,174,478 | overload | 514,256 | 1,048,825 | overload | 9,014 | **0** |
| 16 | 1024 | 934,940 | 1,256,042 | overload | 374,074 | 401,513 | overload | 10,148 | 10,935 |
| 16 | 4000 | 580,052 | 1,135,464 | overload | 229,816 | 451,326 | overload | 9,642 | 9,640 |

出现任一 drop/fallback/spill exhaustion 时不计算正式 ratio，表中的 overload 行只保留原始
数据用于定位瓶颈。

## 热点结论

### producer 固定成本已经明显下降

最有代表性的无 overload 场景：

- 1 thread / 4KiB：producer +48.7%，end-to-end +48.7%；
- 1 thread / 256B：+21.8%；
- 1 thread / 1KiB：+8.0%；
- 4 threads / 64B：+44.4%。

因此上一版 long-message 慢并不只是“spill pool 小”，`spill_mu` 与二次 text scan
确实是实际 hot path。

### 256B inline 问题已解决

4/16 producer 的 256B candidate 都是：

```text
spill exhaustion = 0
drop = 0
```

上一版分别出现 331 / 9014 次 spill exhaustion（本次同 runner tuning baseline 数据）。

说明 512B inline 是有效的容量/性能折中。

### 下一瓶颈是 long-message burst backpressure

1KiB/4KiB 在高 producer 数下仍会耗尽 1024 spill blocks。

值得注意的是 candidate producer 更快，例如：

- 4 threads / 1KiB：430k -> 789k producer/s；
- 16 threads / 4KiB：580k -> 1.136M producer/s。

producer 加速后，单 consumer/backend 更容易在 burst 中落后，所以固定 1024 spill
capacity 更快暴露出来。这是 **backpressure/capacity**，不再是原先共享 mutex 热点。

## 为什么本轮不直接改成 2048 spill

在当前 1272B slot 下，如果 spill 从 1024 提升到 2048：

```text
slots: 1272 * 8192
spill: 4096 * 2048
total: 约 17.94 MiB
```

相对 pre-compact 38.06 MiB 只剩约 **52.9%** 内存下降，已经非常接近 50% 门禁。

这会用额外约 4MiB 固定内存换取更大的有限 burst buffer，却不会提高 single consumer 的
持续处理速率。对持续 overload，pool 最终仍会耗尽。

因此本轮保留 1024 block；下一步应先测量/优化 consumer/backpressure，再决定是否需要
可配置的 burst capacity，而不是用固定内存把瓶颈向后推。

## 正确性验证

runtime candidate 已通过：

- native Release + shared ABI inspection；
- native Debug；
- ASan/UBSan；
- TSan。

TSan 中的 MPSC 回归现在实际使用 8 producer × 700B long messages，共 24000 records，
覆盖 atomic bitmap claim/release、spill publication、block reuse 和 queue rollover。

结论：本轮 bitmap ownership 协议未发现 data race，public API/ABI 无变化。
