# 队列紧凑存储基准测试 验证

日期：2026-09-24

本报告记录 紧凑队列 合并后的首次同 运行器前后对比 实测。吞吐数字来自共享
GitHub Actions 运行器，只用于当前实现之间的相对观察，不构成固定性能承诺。

## 环境

- GitHub Actions run: `36005805057`
- job: `compare`
- 运行器: Ubuntu 24.04.5
- 运行器镜像: `ubuntu-24.04 / 20260907.300.1`
- compiler: GCC 13.3.0
- build: 发布 / static
- baseline commit: `23504f104e900419e891b57d7187ca0bcabffdf3`
- 候选版本 commit: `68463b56ffbf5cd815e37bcc7fb1d6d3adae8bb7`
- 每个组合重复 3 次，取 median
- 每线程 1000 records

## 默认 队列 内存

默认 队列 capacity = 8192。

| 指标 | baseline | 候选版本 |
|---|---:|---:|
| 队列 槽位 | 4872 B | 1016 B |
| 槽位 storage | 39,911,424 B | 8,323,072 B |
| 溢出区 storage | 0 | 4,198,400 B |
| total 队列 preallocation | 39,911,424 B / 38.06 MiB | 12,521,472 B / 11.94 MiB |
| long-message 溢出区 capacity | 不适用 | 1024 × 4100 B |

总预分配内存下降 **68.63%**，超过 CI 要求的 50%，门禁通过。

单个 槽位 从 4872 B 降为 1016 B，下降约 **79.15%**。候选版本 的固定 溢出区
storage 约 4.00 MiB，因此默认配置最终总下降低于单 槽位 降幅，但仍显著。

## 吞吐实测

只有 baseline/候选版本 都没有 drop、sync fallback、溢出区 exhaustion 的组合才计算
ratio。出现 over加载 的组合保留原始数字，但不把 throughput ratio 解释成性能差异。

| threads | bytes | base prod/s | new prod/s | prod ratio | base e2e/s | new e2e/s | e2e ratio | drop | fallback | 溢出区 exhaust |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 | 736,763 | 1,046,523 | 1.420x | 592,150 | 788,883 | 1.332x | 0 | 0 | 0 |
| 1 | 256 | 737,237 | 732,539 | 0.994x | 576,874 | 658,844 | 1.142x | 0 | 0 | 0 |
| 1 | 1024 | 603,972 | 550,038 | 0.911x | 505,370 | 549,842 | 1.088x | 0 | 0 | 0 |
| 1 | 4000 | 708,680 | 260,765 | 0.368x | 464,157 | 260,722 | 0.562x | 0 | 0 | 0 |
| 4 | 64 | 1,172,438 | 1,359,205 | 1.159x | 879,611 | 1,312,408 | 1.492x | 0 | 0 | 0 |
| 4 | 256 | 1,149,161 | 1,227,302 | over加载 | 881,588 | 871,511 | over加载 | 1,107 | 0 | 1,107 |
| 4 | 1024 | 1,148,635 | 1,022,598 | over加载 | 723,596 | 771,512 | over加载 | 713 | 0 | 713 |
| 4 | 4000 | 1,153,436 | 479,250 | 0.415x | 704,679 | 479,193 | 0.680x | 0 | 0 | 0 |
| 16 | 64 | 1,525,873 | 2,014,060 | 1.320x | 863,452 | 1,314,330 | 1.522x | 0 | 0 | 0 |
| 16 | 256 | 1,464,805 | 1,829,910 | over加载 | 802,622 | 666,376 | over加载 | 10,173 | 0 | 10,173 |
| 16 | 1024 | 1,480,498 | 1,509,867 | over加载 | 759,734 | 559,575 | over加载 | 10,007 | 0 | 10,007 |
| 16 | 4000 | 1,391,627 | 827,099 | over加载 | 673,643 | 256,879 | over加载 | 11,007 | 0 | 11,007 |

## 结论

### 1. 内存优化目标达成

默认 队列 固定预分配从 38.06 MiB 降到 11.94 MiB，证明 compact 槽位 方向成立。

64B short-message 路径在本次 运行器 上同时提升 生产者 和 end-to-end throughput，
说明缩小 槽位/cache footprint 对常见短日志有实际收益。

### 2. 256B 内联 阈值偏小

当前 `LOGGER_QUEUE_INLINE_TEXT = 256`，因此 **正文长度正好 256B 已进入 溢出区 path**。

1 thread 下没有问题，但 4/16 生产者 时分别出现 1,107 / 10,173 次 溢出区 exhaustion，
说明 256B 边界会让中等长度日志过早进入共享 溢出区 pool。

下一轮优先评估把 内联 storage 提升到 **512B**。这仍能保留显著内存下降，同时可让
256B 档完全绕开 溢出区 path。

### 3. long-message 溢出区 path 仍有明显成本

4000B 在 1 thread 和 4 threads 下没有 溢出区 exhaustion，但 生产者 throughput 分别只有
baseline 的 0.368x / 0.415x，end-to-end 为 0.562x / 0.680x。

因此这里不能只归因于 溢出区 capacity；当前 `spill_mu` 空闲链表 和 long-message path
本身仍有优化空间。

下一轮应优先减少每条 long message 的共享 mutex 成本，再决定是否扩大 溢出区 capacity。

### 4. 不能通过无限增大 溢出区 pool 解决问题

16 生产者 下 256/1024/4000B 都出现大量 溢出区 exhaustion。简单把 溢出区 pool 扩到
队列 capacity 虽能减少 drop，却会重新引入接近旧实现的固定内存成本。

下一轮调优应同时约束：

- 默认 队列 总预分配仍至少比 baseline 下降 50%；
- 256B 常见消息尽量走 内联；
- long-message 空闲链表 降低共享锁成本；
- 溢出区 capacity 调整必须通过同一 benchmark workflow 复测；
- over加载 场景继续遵守现有 DROP / SYNC policy，不允许 silent truncation。

## 验证状态

同一 候选版本 的常规 CI：

- native 发布 + shared ABI inspection：PASS
- native Debug：PASS
- ASan/UBSan：PASS
- TSan：PASS

benchmark memory gate：PASS。

原始样本由该 workflow 的 `queue-benchmark-comparison` artifact 保存。
