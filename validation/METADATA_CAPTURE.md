# Demand-driven producer metadata 验证

日期：2026-09-25

本报告验证 PR #15 的 demand-driven metadata capture。

目标是删除 producer 中最终 formatter 不会使用的 metadata 采集/复制，不改变 public API、
输出格式、queue topology、bounded-memory、backpressure、flush 或 Audit contract。

## 对比版本

- memory baseline:
  `23504f104e900419e891b57d7187ca0bcabffdf3`
- throughput baseline:
  `6b926d59cda3bf176baa10b24e2c84bc49ce7943`
  - PR #14 self-paced wakeup 合并后的 main
- candidate:
  `2e6ecf0ee496067901d24f5a1ca33cb5019b9005`

benchmark run：`36086853265`。
Ubuntu 24.04.5 / GCC 13.3.0 / Release static。
每组合重复 3 次，每线程 1000 records，取 median。

## Capture policy

实例创建时将 immutable `detail/include_*` 编译为 private `capture_mask`：

```text
MINIMAL
  timestamp + level + text

NORMAL
  + module

VERBOSE
  + configured pid/tid

DEBUG
  + context
  + configured source
```

因此：

- MINIMAL 不执行每条 pid/tid/context/source 采集；
- NORMAL 不执行 pid/tid/context/source；
- VERBOSE 不再采集 DEBUG-only context/source；
- DEBUG 保持原有可见内容；
- PID 在实例创建时缓存；
- TID 仍只在配置真正要求时执行 `SYS_gettid`。

async queue 只复制 mask 指定的 module/context/source；固定 slot layout 不变。

## Memory

默认 queue capacity=8192：

```text
pre-compact: 38.06 MiB
PR #14:      13.94 MiB
candidate:   13.94 MiB
```

candidate 相对 pre-compact 下降 **63.38%**，memory gate PASS。

本轮没有通过缩小 slot 偷换内存收益；优化只减少实际 producer/queue memory traffic。

## Throughput

无 overload 场景：

| threads | bytes | producer ratio | end-to-end ratio |
|---:|---:|---:|---:|
| 1 | 64 | 0.939x | 1.041x |
| 1 | 256 | 1.129x | 1.059x |
| 1 | 1024 | 1.035x | 1.039x |
| 1 | 4000 | 0.996x | 1.003x |
| 4 | 64 | 1.064x | 1.158x |
| 4 | 256 | 1.128x | 1.112x |
| 16 | 64 | 0.983x | 1.128x |
| 16 | 256 | 1.132x | 1.089x |

高并发 1KiB/4KiB 存在既有 spill exhaustion，因此不把 ratio 当作有效持续吞吐对比。

### 可确认的收益

256B 档在多个并发度上表现一致：

- 1 thread producer +12.9%；
- 4 threads producer +12.8%；
- 16 threads producer +13.2%。

4T/64B producer +6.4%，end-to-end +15.8%。

这与设计目标一致：默认 `VERBOSE` 不再复制 DEBUG-only context/source，而短/中消息中
metadata 固定成本占比更高。

### 不应过度解读的结果

1T/64B producer 为 0.939x，16T/64B 为 0.983x，但对应 end-to-end 分别是
1.041x / 1.128x。共享 runner 的 producer timing 与 worker 调度存在明显噪声，因此不能
据单个 producer ratio 宣称稳定 regression。

4KiB 单线程基本持平：

```text
producer 0.996x
e2e      1.003x
```

说明大正文场景仍主要受 formatting/copy/backend 路径控制；继续在 metadata 上堆优化的收益
不会大。

## Test hardening

本轮 CI 过程中还暴露两个测试夹具问题，均未通过 suppression 绕过：

1. metadata policy 初版测试使用 `/dev/null` file backend 并要求 destroy status=0；
   `fsync(/dev/null)` 可返回 EINVAL，错误地把 durability 语义混进纯 policy 测试。
   最终改为不产生 I/O 的 stderr-only fixture。

2. `explicit_cancel_queue` 仍使用旧的 `pthread_cond_signal` 作为 deterministic
   enqueue probe。PR #14 self-paced wakeup 后 signal 已是可选 slow path，调度变化会让该
   测试失去 rendezvous。最终改为 wrap 稳定的 `logger_queue_push()` 边界，与
   `global_cancel_queue` 的测试策略一致。

第二项解释了此前 TSan 的 finished-thread leak：不是 production data race，而是旧测试等待
一个不再保证出现的 optional signal，导致 cancellation 生命周期被测试自身打乱。

## Correctness / CI

最终 candidate `2e6ecf0...` 已通过：

- native Release；
- shared ABI inspection；
- native Debug；
- ASan/UBSan；
- TSan；
- queue benchmark memory gate。

现有 DEBUG format/context/source-ownership 回归继续验证：

- context 可见内容不变；
- source basename/function 有界 snapshot；
- async record 不持有可卸载 SDK source pointer。

新增 `metadata_capture_regression` 验证 MINIMAL/NORMAL/VERBOSE/DEBUG 与
`include_pid/include_tid/include_source` 的 capture-mask 映射。

## 结论

demand-driven metadata 可以保留。

本轮证明继续在 producer metadata 上做复杂优化（例如立即引入 TLS TID cache）的优先级已经
下降。下一阶段应按 Architecture roadmap 做 **consumer-stage profiling**，分别量化：

```text
queue drain / record reconstruction
format
stderr
file
syslog
completion
```

再决定后续是优化 format、batch、Syslog `sendmmsg()`，还是需要更深的 topology 调整。
