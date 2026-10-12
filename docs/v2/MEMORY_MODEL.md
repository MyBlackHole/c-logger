# c-logger 2.0 C11 内存模型

状态：**#103 设计基线，适用于当前不含 Audit 的 ABI 2**。

当前设计入口见 [README.md](README.md)；已退役 Audit 发布/失败策略不再是本版本协议或测试证据。

2.0 的用户态并发首先服从 ISO C11 数据竞争规则。Linux 内核中的发布、RCU、seqcount、屏障等思想可以借鉴，但不能直接复制仅适用于内核的内存模型假设。

## 1. 基本规则

- `volatile` 不是同步、不提供原子性、不是内存屏障。
- 普通非原子对象存在并发读写且没有锁/同步时，即使最终会重试，也属于 C11 数据竞争。
- `memory_order_relaxed` 只提供原子性和该原子对象的修改顺序/一致性，不发布依赖负载。
- release/acquire 只有建立在明确的 synchronizes-with / happens-before 边上时才用于负载发布。
- 互斥锁/读写锁的加锁/解锁是同步边；由锁保护的普通对象不需要再人为变成原子对象。
- 原子状态不能替代对象生命周期；先证明指针仍然可解引用，再谈原子加载/CAS。
- `seq_cst` 栅栏不是“更安全”的装饰；必须有明确的唤醒丢失/顺序证明和试金石测试才允许使用。

<a id="2-helper-设计原则"></a>

## 2. 辅助函数设计原则

当前不引入泛化的 `load_acquire(x)` / `store_release(x)` 宏来隐藏 C11 原子 API。

原因：

- 不同原子对象的内存序目的不同；
- 泛型包装容易让审查者看不到发布点；
- 很多 relaxed 操作是有意的位置预留/指标统计，而不是缺少 acquire/release。

优先采用**协议语义辅助函数**，例如未来可以考虑：

```text
queue_slot_publish()
queue_slot_observe()
queue_slot_release_for_reuse()
logger_stop_publish()
global_ticket_publish()
```

这些辅助函数必须在注释中写明对应 happens-before 关系。

<a id="3-queueslot-publication"></a>

## 3. 队列：槽位发布

当前队列使用 MPSC 位置预留 + 单消费者槽位代次。

<a id="producer"></a>

### 生产者

```text
enqueue_pos relaxed CAS
    -> 独占逻辑位置
    -> 写普通 slot.record 负载
    -> slot.seq release store(pos + 1)
```

`enqueue_pos` 只负责预留唯一性，不发布负载。

真正的记录发布点是：

```c
atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
```

<a id="consumer"></a>

### 消费者

消费者只有在：

```c
atomic_load_explicit(&slot->seq, memory_order_acquire) == pos + 1
```

之后才读取普通 `slot.record`。

因此：

```text
生产者写入负载
    -> slot.seq release
    -> slot.seq acquire
    -> 消费者读取负载
```

构成 C11 happens-before，普通负载不发生数据竞争。

<a id="4-queueslot-reuse"></a>

## 4. 队列：槽位复用

消费者将完整记录复制到工作线程批次，并归还该记录的溢出块后，即发布槽位可复用；此时尚未执行后端输出：

```c
atomic_store_explicit(&slot->seq, pos + q->cap, memory_order_release);
```

下一轮生产者在覆写槽位前 acquire 加载对应 `slot.seq`。

因此旧消费者的负载读取与新生产者的覆盖写入通过代次原子变量建立顺序。

不得把 `enqueue_pos/dequeue_pos` 当成槽位负载发布原语。

<a id="5-spill-ownership"></a>

## 5. 溢出块所有权

`spill_used[]` 是所有权位图，不是正文发布通道。

获取溢出块：

```text
生产者 CAS 0 -> 1 (acq_rel)
    -> 当前生产者独占溢出块
    -> 写溢出正文
    -> 通过 slot.seq release 发布记录
```

消费者以 acquire 读取 slot.seq 后才读溢出正文。

消费者将溢出块正文复制到独立工作线程批次后，在槽位可复用之前归还块：

```text
以 release 清零 spill_used
    -> 下一生产者执行 acquire CAS
    -> 允许覆盖该溢出块
```

因此溢出块的两个关键关系是：

1. 内容发布由 `slot.seq` 完成；
2. 块复用顺序由 `spill_used` release/acquire 完成。

<a id="6-queue-sleepwakeup"></a>

## 6. 队列睡眠/唤醒

#103 第二阶段确实将旧的双 SC 栅栏握手替换为**同一个原子变量上的
acq_rel RMW 交接**。但 #116 曾以“最小双栅栏模型在 C11 下不可证明”
为理由，这个否定依据已撤回：C11 草案 N1570 §7.17.3 第 11 段
能够证明明确前提下的最小模型。该结论不自动证明旧完整 MPSC 协议，
也不意味着当前 RMW 实现错误或必须回退：

```text
工作线程：                           生产者：
exchange(waiting, 1, acq_rel)        发布 slot.seq (release)
重新检查 slot.seq (acquire)          fetch_or(waiting, 0, acq_rel)
                                      |
                                      +-- 看到 1 -> 在 wait_mu 下清零并通知
```

核心不是“RMW 更强”，而是 `consumer_waiting` 自己有唯一修改顺序，
且每个 RMW 都读取该顺序中紧邻自身之前的修改所写入的值。初始化后，所有并发的
写入都通过 acq_rel RMW 辅助函数：

- `logger_queue_wait_arm_recheck()`；
- `logger_queue_wait_probe_after_publish()`；
- `logger_queue_wait_claim_signal()`；
- `logger_queue_wait_disarm()`。

因此只有两类情况：

1. 生产者探测在工作线程建立等待准备之前：建立等待准备的 acquire 操作通过 RMW 链取得生产者
   release 之前的 `slot.seq` 发布，工作线程重新检查不能继续把该记录
   当作未发布；
2. 工作线程建立等待准备在生产者探测之前：探测观察到 waiting=1 并进入
   `wait_mu + cond_signal` 慢速路径；如果中间已经有人把 1 清为 0，那么该
   修改本身必须来自已证明安全的通知/解除等待准备/强制唤醒路径。

这里的第 1 条对队列为空判定应理解为“**当前出队队首的生产者**
发布已进入交接链”。若后续位置先发布、队首位置
仍有预留空洞，工作线程仍可看到队首为空；但真正关闭队首空洞的生产者
在发布后也会执行探测，并在工作线程已建立等待准备时负责通知。该边界在正式
证明与试金石测试的预留空洞场景中单独验证。

当前实现不再使用 `atomic_thread_fence(memory_order_seq_cst)`，
**不是因为旧最小握手违反了 C11**。对于单生产者、单次 arm/发布且无中途清零
的模型，C11 [N1570 §7.17.3 第 11 段](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
分别排除“先发布栅栏却重查不到发布”和“先 arm 栅栏却探测不到 arm”；
两侧原子操作本身无需 seq_cst。完整 MPSC 仍需处理连续周期、
reservation gap、清零通知者和关闭，不能从最小模型直接推广。

保留当前 RMW 是为了将多生产者发布交接集中在单原子修改顺序与
release/acquire 链上，便于证明通知职责和中途清零；不声称旧方案
已被证明错误，也不以 C++ 后来的规则作为 C11 前提。
RMW 额外争用的直接性能证据仍由
[#128](https://github.com/MyBlackHole/c-logger/issues/128) 跟踪。
#193/#194 解决了基准构建口径和实际输入测量，但尚非旧双栅栏与 RMW
的同环境生产等价前后对照。

正式证明、MPSC RMW 链、条件变量等待入口、关闭和验证边界见：

- `docs/v2/QUEUE_WAKEUP_PROOF.md`；
- `tests/regression/test_queue_wakeup_litmus.c`。

试金石测试/压力测试/TSan 是证明证据，不替代 C11 证明。

<a id="7-worker-stop-state"></a>

## 7. 工作线程停止状态

`running` 是工作线程停止/准入状态，不是 `logger_t` 生命周期固定保护（pin）。

```text
所有者以 release 存储 running=0
    -> 唤醒工作线程
    -> 工作线程以 acquire 加载 running
    -> 工作线程退出
    -> pthread_join
    -> 允许最终内存回收
```

真正证明工作线程不再访问父对象内存的机制是 `pthread_join()`，不是 `running=0`。

<a id="8-logger-state"></a>

## 8. Logger 状态

`logger_t.state` 只表示 API 准入/状态机。

- RUNNING -> STOPPING/STOPPED 使用 release 存储；
- API 通过 acquire 加载判断是否接受操作；
- 它不保护调用方手中的裸 `logger_t *` 生命周期。

外部调用方生命周期仍由 #102 所有权契约保证。

<a id="9-completion-counters"></a>

## 9. 完成计数器

`async_completed/sync_completed` 等完成计数用于观察进度。

`completed_pos` 是 `progress_mu` 保护的条件谓词，`progress_cv` 必须与该互斥锁配对。

规则：

- 条件等待必须 `while (predicate not satisfied) pthread_cond_wait(...)`；
- 不能把一次通知/广播当成完成本身；
- 队列出队只表示消费者已复制记录并归还槽位/溢出块，不等于后端输出完成；
- 工作线程在 `emit_batch()` 返回后推进 `async_completed`，同步协议正常时再持 `progress_mu` 推进 `completed_pos`；
- 后端 I/O 失败也会推进完成进度并记录错误；完成不是成功或持久化证明。

当前 flush 水位是基于谓词的进度机制，不等同于 Linux 一次性 `completion`。

因此 #103 暂不为了名称一致强行引入通用完成原语。

<a id="10-global-publication--lifetime"></a>

## 10. Global 发布/生命周期

`g_ticket` 是代次 + 阶段准入令牌。

`g_logger` 是普通指针，真正由 `g_lifetime_lock` 保护。

Global 初始化的 STARTING/RUNNING 及关闭的 STOPPING/STOPPED 令牌 CAS
统一使用 success=`memory_order_acq_rel`、failure=`memory_order_acquire`。这样保留成功路径的
release 发布与失败路径的 acquire 观察，不依赖历史 C11 “failure 不强于 success”
对 release/acquire 组合的解释歧义。这是契约/可移植性修复，不是已复现 UB 的结论。
`g_ticket` 仍只负责代次/准入，不提供 `g_logger` 的生命周期固定保护（pin）；
原子内存序静态检查不能替代 happens-before 与生命周期证明。

发布：

```text
获取生命周期写锁
    -> g_logger = candidate
    -> 发布 RUNNING 令牌
    -> 释放生命周期锁
```

读者：

```text
读取令牌
    -> 获取生命周期读锁
    -> 重新检查令牌
    -> 持锁期间读取 g_logger
```

关闭：

```text
发布 STOPPING 令牌
    -> 生命周期写锁等待读者排空
    -> g_logger = NULL
    -> 解锁
    -> 销毁旧 Logger
```

重要结论：

> `g_ticket` 不是 `g_logger` 的内存回收原语；读写锁才是当前生命周期固定保护（pin）。

不能把该模型直接替换成 `atomic g_logger + refcount_inc_not_zero()`，因为调用方在 CAS 引用计数前仍需要证明指针指向的内存/引用计数字段的存储尚未被释放。

<a id="11-console-config"></a>

## 11. Console 配置

`g_config` 把详细程度 + 颜色放在一个原子快照中。

操作只要求拿到某个完整配置快照，不依赖其他负载发布，因此 relaxed 加载/存储/CAS 是合理的。

如果未来配置拆成多个字段，则必须重新设计一致快照，不能用多个 relaxed 原子假装事务。

<a id="12-process-atomics"></a>

## 12. 进程原子变量

`owner_pid/forked_child/live_objects` 属于进程防护，不是对象生命周期发布。

- `live_objects` 是存活对象计数；
- `owner_pid` 是进程身份；
- `forked_child` 是继承运行时的拒绝标记位。

它们不能被当作 Logger 指针内存回收原语。

<a id="13-metrics-atomics"></a>

## 13. 指标原子变量

纯统计计数默认使用 relaxed。

只有当某计数器同时承担“事件完成/依赖负载可见性”的协议职责时，才允许提升到 release/acquire，并必须在本文登记。

不要因为诊断读取用了 acquire 就推断其他普通字段随之安全；普通字段仍需要各自的生命周期/锁契约。

## 14. seqcount 结论

当前不引入 seqcount。

原因：

- C11 下读者无锁读取写者正在修改的普通负载会构成数据竞争，即使 seq 变化后重试；
- 现有指标大多已经是独立原子变量；
- 后端可变状态已由互斥锁保护；
- 当前没有已证明的读多写少的一致快照热点值得承担复杂度。

以后如需类似 seqcount 的方案，必须先给出 C11 合法的数据表示，不能照搬 内核 seqcount 对普通负载的假设。

## 15. RCU/SRCU 结论

当前不引入 RCU/SRCU。

Global 可能是未来候选，但在 #104 决定 Global 外观接口/对象模型前，现有读写锁生命周期证明更简单。

任何未来 RCU 方案必须同时解决：

- 读者登记/宽限期；
- 内存回收；
- fork 子进程；
- 取消；
- DSO 卸载（dlclose）；
- 指针发布；
- 基准测试证明读写锁是真实瓶颈。

<a id="16-review-checklist"></a>

## 16. 审查清单

任何新的无锁访问/原子协议必须回答：

1. 哪些内存位置是原子，哪些是普通负载？
2. 谁写负载？
3. 线性化点是什么？
4. 用于发布的原子对象是什么？
5. release 在哪里？
6. acquire 在哪里？
7. 哪条边形成 happens-before？
8. 内存回收由什么保证？
9. ABA 是否可能？
10. 进度保证是可阻塞/无锁（lock-free）/无等待（wait-free）中哪一种？
11. 失败/重试是否会重复副作用？
12. 有什么试金石测试/TSan/故障注入证据？
