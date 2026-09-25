# Architecture Invariants

本文是 c-logger 的架构 review gate。

它不描述“当前代码具体怎么写”，而定义**局部重构、性能优化、backend 扩展不能破坏的性质**。

总体组件与数据流见 `ARCHITECTURE.md`。

## 1. Host / Integration

1. Logger 不接管宿主 process/thread/fork model。
2. 第三方 SDK 默认不偷偷创建或销毁 global Logger。
3. explicit `logger_t` 由 host structural owner 独占销毁。
4. SDK/caller 对 explicit instance 默认是 BORROWED。
5. public ABI 保持 plain C；内部 GNU C helper 不泄漏进 public contract。
6. 新 hidden thread/process-global state 必须先做 architecture review。

## 2. Ownership / Lifetime

7. 每个非平凡资源必须回答：
   - 谁创建；
   - 当前谁拥有；
   - 何时 transfer；
   - 最终谁 release。
8. BORROWED pointer 不能被 lexical cleanup 自动释放。
9. move 后旧 owner 必须失效。
10. worker/queue/workspace 必须 join-before-free。
11. destroy consumes ownership。
12. final I/O failure 不让旧 pointer 变成可重试对象。
13. lock、atomic、lifetime pin、join、refcount 不得互相替代。
14. generic refcount 只有在多个独立 owner 真正需要 retain lifetime 时才允许引入。

## 3. Async / Queue

15. async queue 必须 bounded。
16. 当前 compatibility async path 不允许 per-record heap allocation。
17. 成功 enqueue 后，record 不依赖 caller/SDK source pointer 生命周期。
18. overflow 必须通过明确 policy 表达。
19. 不允许 silent truncation 代替 resource exhaustion。
20. 不允许无限增长 queue/spill 来掩盖持续 consumer overload。
21. queue topology 可以改变，但必须保持可证明的 publication/order/completion 语义。
22. performance queue 修改必须同时检查固定内存成本。

## 4. Completion / Flush

23. dequeue 不等于 backend completion。
24. queue empty 不等于 backend completion。
25. backend completion 不自动等于 durable fsync。
26. flush 必须等待其 snapshot target 的 completion。
27. flush 语义变化属于 architecture/API contract change，不能只改实现代码。

## 5. Backend / I/O

28. backend mutable state 必须有唯一 serialization contract。
29. file target ownership 不能被多个独立 Logger 无协议共享。
30. reopen/rotation 必须先准备并验证 replacement，再 retire usable old state。
31. no-clobber rotation 不能为兼容平台退化成覆盖式 rename。
32. error-bearing finalization 保持显式。
33. first meaningful backend error 不能被 cleanup error 覆盖。
34. sticky historical I/O error 不能被后续成功静默清除。
35. unsupported platform capability 返回明确错误，不做危险降级。
36. Syslog failure/backpressure 不能伪装成 durable success。

## 6. Process / Cancellation

37. raw fork child 不得触碰可能处于 inherited-locked 状态的复杂 runtime。
38. fork safety 不能通过重置继承 pthread object 来伪造。
39. library 不扫描/控制整个宿主线程模型来替宿主决定生命周期。
40. cancellation 恢复前必须先释放库内 lock/resource/pin。
41. 普通 API 不宣称 signal-safe 或 generic async-cancel-safe。
42. `pthread_exit/longjmp` 不能成为绕过 cleanup/lifetime protocol 的支持路径。

## 7. Audit

43. Audit 是独立 security/durability subsystem，不等价于普通 Logger。
44. Audit single-writer ownership 必须在 recovery/active use 前建立。
45. crypto failure 必须 fail closed。
46. 不支持的 algorithm 不自动映射到另一个 algorithm。
47. log commit 与 checkpoint success 是不同状态。
48. uncertain I/O/crypto state 必须可观察。
49. recovery 不静默伪造、覆盖或重写历史可信链。
50. 普通 Logger throughput 优化不得弱化 Audit durability/security contract。

## 8. Performance

51. 性能结论必须有可复现 benchmark 证据。
52. shared runner 的吞吐抖动不应成为脆弱的硬门禁。
53. correctness、bounded memory、lifetime 优先于单项 logs/sec。
54. “lock-free”标签不能替代 ownership/publication proof。
55. 不能通过 drop 更多记录制造虚假 throughput improvement。
56. 提高固定内存换 burst capacity 时，必须同时报告 memory tradeoff。
57. 新 fast path 不得偷偷改变旧 compatibility API 的 caller-lifetime 语义。

## 9. Current Implementation vs Invariant

以下只是当前实现，可以在满足上述规则时改变：

| 当前实现 | 是否必须长期保留 |
|---|---|
| shared MPSC | 否 |
| single async worker | 否 |
| 512B inline | 否 |
| 1024 spill blocks | 否 |
| atomic spill bitmap | 否 |
| BATCH_MAX=256 | 否 |
| eager vsnprintf | 否 |
| per-record worker notification | 否 |
| Syslog per-record send | 否 |

以下性质属于当前核心 invariant：

| 性质 | 必须保留 |
|---|---|
| host-owned explicit instance | 是 |
| bounded async memory | 是 |
| explicit overflow policy | 是 |
| no silent overflow truncation | 是 |
| async source lifetime isolation | 是 |
| owner-controlled worker lifetime | 是 |
| join-before-free | 是 |
| flush != queue empty | 是 |
| visible backend failure | 是 |
| file ownership/no-clobber contract | 是 |
| Audit fail-closed durability model | 是 |

如果未来确实要改变后一类性质，应视为**架构/API contract 变更**，不能作为普通性能重构合入。

## 10. Architecture Review Checklist

涉及 queue、worker、backend、lifecycle、资源或 public API 的修改，至少回答：

1. 是否改变 Host/SDK ownership 边界？
2. 是否增加 hidden thread 或 process-global state？
3. 是否改变 bounded-memory 上界？
4. 是否改变 caller/source lifetime？
5. 是否改变 overflow/backpressure？
6. 是否改变 record ordering？
7. 是否改变 flush/completion？
8. 是否新增 lock order？
9. atomic 是否被误当 lifetime mechanism？
10. 是否改变 final release 顺序？
11. 是否隐藏 I/O/durability error？
12. 是否影响 fork/dlclose/cancellation？
13. 是否影响 Audit security/durability？
14. 是否需要 public ABI/version change？
15. 性能收益是否有同 runner before/after evidence？
16. 是否通过更多 drop、更弱语义或更多固定内存换取表面 throughput？
17. 对应专项文档、tests、benchmark 是否同步更新？

这些问题没有回答清楚之前，不应把局部实现优化视为架构上完成。
