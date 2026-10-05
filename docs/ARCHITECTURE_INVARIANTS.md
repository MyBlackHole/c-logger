# 架构不变量

本文是 c-logger 的架构 评审门禁。

它不描述“当前代码具体怎么写”，而定义**局部重构、性能优化、后端 扩展不能破坏的性质**。

总体组件与数据流见 `ARCHITECTURE.md`。

## 1. 宿主 / 集成

1. Logger 不接管宿主 进程/线程/fork 模型。
2. 第三方 SDK 默认不偷偷创建或销毁 global Logger。
3. explicit `logger_t` 由 host 结构性所有者 独占销毁。
4. SDK/caller 对 explicit 实例 默认是 BORROWED。
5. public ABI 保持 plain C；内部 GNU C helper 不泄漏进 公开契约。
6. 新 隐藏的线程/进程全局状态 必须先做 架构评审。

## 2. 所有权 / 生命周期

7. 每个非平凡资源必须回答：
   - 谁创建；
   - 当前谁拥有；
   - 何时 转移；
   - 最终谁 release。
8. BORROWED 指针 不能被 词法作用域清理 自动释放。
9. 移交 后旧 所有者 必须失效。
10. 工作线程/队列/工作区 必须 释放前等待退出。
11. 销毁 消费s 所有权。
12. 最终 I/O 失败 不让旧 指针 变成可重试对象。
13. lock、atomic、生命周期固定、join、refcount 不得互相替代。
14. 通用引用计数 只有在多个独立 所有者 真正需要 延长生命周期 时才允许引入。

## 3. 异步 / 队列

15. async 队列 必须 bounded。
16. 当前 compatibility async path 不允许 per-record heap allocation。
17. 成功 en队列 后，record 不依赖 caller/SDK 源指针 生命周期。
18. 溢出 必须通过明确 policy 表达。
19. 不允许 silent truncation 代替 resource exhaustion。
20. 不允许无限增长 队列/溢出区 来掩盖持续 消费者 overload。
21. 队列拓扑 可以改变，但必须保持可证明的 发布/顺序/完成 语义。
22. performance 队列 修改必须同时检查固定内存成本。

## 4. 完成 / 刷新

23. 出队 不等于 后端完成。
24. 队列为空 不等于 后端完成。
25. 后端完成 不自动等于 durable fsync。
26. 刷新 必须等待其 快照目标 的 completion。
27. 刷新 语义变化属于 architecture/API contract change，不能只改实现代码。

## 5. 后端 / I/O

28. 后端 mutable state 必须有唯一 串行化契约。
29. file target 所有权 不能被多个独立 Logger 无协议共享。
30. reopen/rotation 必须先准备并验证 替代对象，再 retire usable old state。
31. no-clobber rotation 不能为兼容平台退化成覆盖式 rename。
32. error-bearing finalization 保持显式。
33. first meaningful 后端 error 不能被 cleanup error 覆盖。
34. sticky historical I/O error 不能被后续成功静默清除。
35. unsupported platform capability 返回明确错误，不做危险降级。
36. Syslog 故障/backpressure 不能伪装成 durable success。

## 6. Process / Cancellation

37. raw fork 子进程 不得触碰可能处于 inherited-locked 状态的复杂 运行时。
38. fork safety 不能通过重置继承 pthread object 来伪造。
39. library 不扫描/控制整个宿主线程模型来替宿主决定生命周期。
40. cancellation 恢复前必须先释放库内 lock/resource/pin。
41. 普通 API 不宣称 唤醒信号-safe 或 generic async-cancel-safe。
42. `pthread_exit/longjmp` 不能成为绕过 cleanup/生命周期 protocol 的支持路径。

## 7. Audit

43. Audit 是独立 security/持久性 subsystem，不等价于普通 Logger。
44. Audit single-写入器 所有权 必须在 recovery/active use 前建立。
45. crypto 故障 必须 fail closed。
46. 不支持的 algorithm 不自动映射到另一个 algorithm。
47. log commit 与 检查点 success 是不同状态。
48. uncertain I/O/crypto state 必须可观察。
49. recovery 不静默伪造、覆盖或重写历史可信链。
50. 普通 Logger throughput 优化不得弱化 Audit 持久性/security contract。

## 8. Performance

51. 性能结论必须有可复现 benchmark 证据。
52. shared runner 的吞吐抖动不应成为脆弱的硬门禁。
53. correctness、有界内存、生命周期 优先于单项 logs/sec。
54. “lock-free”标签不能替代 所有权/发布 proof。
55. 不能通过 drop 更多记录制造虚假 throughput improvement。
56. 提高固定内存换 burst capacity 时，必须同时报告 memory tradeoff。
57. 新 fast path 不得偷偷改变旧 compatibility API 的 caller-生命周期 语义。

## 9. Current Implementation vs Invariant

以下只是当前实现，可以在满足上述规则时改变：

| 当前实现 | 是否必须长期保留 |
|---|---|
| shared MPSC | 否 |
| single async 工作线程 | 否 |
| 512B inline | 否 |
| 1024 溢出区 blocks | 否 |
| atomic 溢出区 bitmap | 否 |
| BATCH_MAX=256 | 否 |
| eager vsnprintf | 否 |
| self-paced 工作线程 wakeup | 否 |
| Syslog per-record send | 否 |

以下性质属于当前核心 invariant：

| 性质 | 必须保留 |
|---|---|
| host-owned explicit 实例 | 是 |
| bounded async memory | 是 |
| explicit 溢出 policy | 是 |
| no silent 溢出 truncation | 是 |
| async source 生命周期 isolation | 是 |
| 所有者-controlled 工作线程 生命周期 | 是 |
| 释放前等待退出 | 是 |
| 刷新 != 队列为空 | 是 |
| visible 后端 故障 | 是 |
| file 所有权/no-clobber contract | 是 |
| Audit fail-closed 持久性 model | 是 |

如果未来确实要改变后一类性质，应视为**架构/API contract 变更**，不能作为普通性能重构合入。

## 10. Architecture Review Checklist

涉及 队列、工作线程、后端、lifecycle、资源或 公开 API 的修改，至少回答：

1. 是否改变 Host/SDK 所有权 边界？
2. 是否增加 hidden thread 或 process-global state？
3. 是否改变 bounded-memory 上界？
4. 是否改变 caller/source 生命周期？
5. 是否改变 溢出/backpressure？
6. 是否改变 record ordering？
7. 是否改变 刷新/completion？
8. 是否新增 lock order？
9. atomic 是否被误当 生命周期 mechanism？
10. 是否改变 final release 顺序？
11. 是否隐藏 I/O/持久性 error？
12. 是否影响 fork/dlclose/cancellation？
13. 是否影响 Audit security/持久性？
14. 是否需要 public ABI/version change？
15. 性能收益是否有同 runner before/after evidence？
16. 是否通过更多 drop、更弱语义或更多固定内存换取表面 throughput？
17. 对应专项文档、tests、benchmark 是否同步更新？

这些问题没有回答清楚之前，不应把局部实现优化视为架构上完成。
