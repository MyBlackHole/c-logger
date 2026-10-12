<a id="queue-sleepwakeup-c11-proof"></a>

# 队列睡眠/唤醒的 C11 证明

状态：**#103 第二阶段生产契约**。

本文件证明队列的自节奏睡眠/唤醒握手。它不证明整个 MPSC
队列，也不把条件变量当成负载发布原语。

## 1. 目的

生产者在工作线程没有准备睡眠时不获取 `wait_mu`、不调用
`pthread_cond_signal()`，保留自节奏通知；同时必须保证最后一条
记录不会因为唤醒丢失永久留在队列中。

危险结果只有一个：

```text
工作线程重新检查时看不到新记录
并且
生产者看不到工作线程已建立等待准备
并且
工作线程随后进入 cond_wait
```

<a id="2-为什么删除旧-sc-fence-handshake"></a>

## 2. 旧双 SC 栅栏：最小模型成立，不等于完整协议证明

#116 曾以“旧双 SC 栅栏 + 较弱原子不能在 C11 下证明”为由替换协议。
**对最小模型的这一否定依据应撤回**。WG14 DR 407 关于混合内存序的一般警告
不是特定模式的反例；以下推理只依据 C11，不借用后来的 C++ 规则。

### 2.1 操作与最小模型前提

只考虑一个生产者、一个工作线程、一个尚未发布的队首槽位。
waiting 初始为 0，slot.seq 初始为旧代次，二者均为原子对象。

    生产者：   Q  = store_release(slot.seq, 已发布代次)
               Fp = atomic_thread_fence(seq_cst)
               Lw = load_acquire(waiting)

    工作线程： A  = store_release(waiting, 1)
               Fc = atomic_thread_fence(seq_cst)
               Rq = load_acquire(slot.seq)

各线程有程序顺序 Q -> Fp -> Lw、A -> Fc -> Rq。
在这次决定睡眠之前，waiting 没有其他修改或 1->0 清零；
slot.seq 没有其他发布、回收或代次回绕。本模型只判定
“Lw 观察到旧 0 且 Rq 同时观察到旧槽位”能否发生，
不包含后续等待周期、多生产者或信号解析者。

### 2.2 两种 SC 栅栏全序

[C11 草案 N1570 §7.17.3 第 11 段](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
规定：对于同一原子对象 M 的修改 X 和读取 Y，如果 X 先于 SC 栅栏
F1、SC 栅栏 F2 先于 Y，且 F1 在 SC 全序 S 中先于 F2，
Y 只能读取 X 或其修改顺序中更晚的值。
**X 和 Y 本身不要求是 seq_cst 操作**。

- **Fp 在 S 中先于 Fc**：取 M=slot.seq、X=Q、Y=Rq。
  Rq 不能继续看见 Q 之前的旧代次；单次发布且未复用时，
  工作线程重查会发现队首记录。
- **Fc 在 S 中先于 Fp**：取 M=waiting、X=A、Y=Lw。
  Lw 不能继续看见 A 之前的 0；无中间清零时，
  生产者观察到 waiting=1 并承担通知。

Fp、Fc 必有先后，因此在上述最小模型中两个线程不能同时漏看这次写入。
工作线程观察到 Q 的发布值后，负载可见性仍由 slot.seq 的
release/acquire 保证，而不是由栅栏替代负载发布点。

### 2.3 证明边界及保留 RMW 的理由

第 11 段不能自动证明完整 MPSC 协议在以下情况下的正确性：

- 多生产者、后续原子修改、等待位 1->0 的通知职责交接；
- 连续等待周期、队首预留空洞、槽位复用与代次回绕；
- wait_mu/条件变量等待入口、关闭/失败、取消和执行者进度。

**最小模式成立，不等于旧完整协议已经验收，也不意味着应回退当前 RMW。**
保留 RMW 的工程理由是：所有并发修改进入同一个 consumer_waiting
修改顺序，连续的 acq_rel 读改写交接链让多生产者、重复周期和
清零解析者的证明更直接（见第 7～11 节）。额外缓存行写竞争
必须用生产等价的直接前后对照量化，见
[#128](https://github.com/MyBlackHole/c-logger/issues/128)；
不能缺少测量就宣称 RMW 没有性能退化。

WG14 DR 407 与 C++ P0668 仅作为历史背景，既不是最小模式反例，
也不能替代完整生产协议的逐项 C11 证明。

<a id="3-新协议单-atomic-rmw-handoff"></a>

## 3. 新协议：单原子变量 RMW 交接

`consumer_waiting` 仍只表示 0/1：

```text
0 = 工作线程未承诺睡眠
1 = 工作线程在 wait_mu 下已建立等待准备，准备重新检查/cond_wait
```

但并发阶段对它的访问规则改变为：

> 初始化之后，只要生产者/工作线程/强制唤醒仍可能并发，所有修改都必须是
> acq_rel 读-改-写；禁止加入普通原子存储修改该状态。

工作线程：

```text
获取 wait_mu
exchange(waiting, 1, acq_rel)       // 建立等待准备
重新检查队列/running
  有工作/停止 -> exchange(waiting, 0, acq_rel) -> 解锁
  队列为空/继续运行 -> pthread_cond_wait
               ...
exchange(waiting, 0, acq_rel)       // 解除等待准备
解锁
```

生产者：

```text
写入负载
slot.seq = 已发布值（release）
fetch_or(waiting, 0, acq_rel)       // RMW 探测；值本身不变
  old == 0 -> 返回
  old == 1 -> 获取 wait_mu
              exchange(waiting, 0, acq_rel)
                old == 1 -> 通知
                old == 0 -> 已有执行者处理了该等待状态
              解锁
```

关闭：

```text
running = 0
获取 wait_mu
exchange(waiting, 0, acq_rel)
cond_signal                          // 无条件强制唤醒
解锁
```

## 4. C11 基础

本证明只依赖 C11/C17 的三个基础性质：

1. 每个原子对象有自己的修改顺序；
2. 原子读-改-写总是读取该修改顺序中紧邻其写入之前的
   最后一个值；
3. release 操作与读取其值/其释放序列值的 acquire 操作
   建立 synchronizes-with，进而形成 happens-before。

`atomic_exchange_explicit(..., memory_order_acq_rel)` 和
`atomic_fetch_or_explicit(..., memory_order_acq_rel)` 同时承担 acquire 与
release。

<a id="5-payload-publication-不变"></a>

## 5. 负载发布不变

队列记录的线性化/发布点仍然是：

```c
atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
```

消费者只有：

```c
atomic_load_explicit(&slot->seq, memory_order_acquire) == pos + 1
```

后才读取普通 `slot.record`。

唤醒交接不替代 `slot.seq` 发布，只保证工作线程与生产者在“决定是否睡眠”
这一点不会同时漏看对方的状态。

<a id="6-mpsc-reservation-gap-边界"></a>

## 6. MPSC 预留空洞边界

MPSC 的 `enqueue_pos` 只分配位置，不发布负载。因此生产者可以出现：

```text
P0 预留位置 N      // 队首，暂未发布
P1 预留位置 N+1
P1 发布 N+1
P1 探测 waiting
工作线程建立等待准备
工作线程重新检查位置 N  // 仍为空
```

所以“工作线程 acquire 了某个生产者的唤醒交接”**不等于**“当前出队
队首一定已经可消费”。最小证明中的 Q 必须理解为：**填补当前队首空洞的
发布**。

上述空洞不会造成唤醒丢失：

1. 如果队首生产者 P0 在工作线程建立等待准备 **之前** 发布 + 探测，则 P0 的 RMW
   也进入同一交接链，工作线程建立等待准备并执行 acquire 之后重新检查能看到队首
   发布；
2. 如果 P0 在工作线程建立等待准备 **之后** 发布，则它必须在发布后执行自己的
   RMW 探测；工作线程尚需睡眠时 waiting=1，因此 P0 进入通知慢速路径；
3. 如果中间已有生产者、工作线程或强制唤醒路径承担处理职责，把 waiting 清成 0，则由
   第 8 节的等待处理责任规则保证该等待周期已经被解决。

如果某生产者预留了队首位置后永久不再发布，则队列本身已经
因为预留空洞无法前进；这是 MPSC 生产者进度/取消问题，
不是唤醒握手可以修复的问题。生产 Logger API 在运行时作用域
内禁用延迟取消，避免在队列发布临界路径中被取消。

`queue_wakeup_litmus` 的并发核心测试直接建模队首发布者；另有确定性
预留空洞场景验证“后续槽位先发布后，队首发布者仍负责
处理已经建立等待准备的等待者”。

<a id="6-最小两线程-proof"></a>
<a id="7-最小两线程-proof"></a>

## 7. 当前 RMW 协议的最小两线程证明

定义：

```text
Q = 生产者以 release 发布 slot.seq
P = 生产者通过 acq_rel RMW 探测 waiting（fetch_or 0）

A = 工作线程通过 acq_rel RMW 设置 waiting=1，建立等待准备
R = 工作线程以 acquire 重新检查 slot.seq
```

且有程序顺序：

```text
Q sequenced-before P
A sequenced-before R
```

因为 A、P 都是同一个 `consumer_waiting` 上的 RMW，它们在该原子变量的
修改顺序中必有先后。

<a id="case-1-p-在-a-之前"></a>

### 情形 1： P 在 A 之前

如果两者之间没有其他修改，A 必须读取 P 写入的 0。

P 是 release，A 是 acquire，因此：

```text
Q -> P  synchronizes-with  A -> R
```

得到：

```text
Q happens-before R
```

所以 R 对对应 `slot.seq` 的 acquire 观察不能继续观察 Q 之前的
代次；工作线程会看到该记录已发布，不会睡眠。

如果 P 与 A 之间存在其他生产者的 acq_rel RMW，它们逐个 acquire 前驱、
再 release 给后继，形成传递交接链；结论不变。

<a id="case-2-a-在-p-之前"></a>

### 情形 2： A 在 P 之前

没有中间的清零操作时，P 的 RMW 必须读取 A 写入的 1，因此返回
`old == 1`，生产者进入 `wait_mu` 慢速路径。

工作线程从建立等待准备到决定 cond_wait 一直持有同一把 `wait_mu`：

- 工作线程重新检查时若已看到待处理工作或停止状态，会先解除等待准备并解锁；
- 工作线程若仍决定睡眠，`pthread_cond_wait()` 原子释放互斥锁并进入等待；
- 生产者取得互斥锁后通过 1->0 取得通知职责，并发出通知。

所以生产者不会把通知发在工作线程“已经承诺睡、但还没进入等待”的缝隙里。

<a id="8-intervening-0-为什么安全"></a>

## 8. 中间出现的 0 为什么安全

MPSC 下 P 可能在 A 之后，却读到 0；这只能因为 A 与 P 之间已经出现新的
修改。生产环境中能把 1 变成 0 的路径只有：

1. **生产者取得通知职责**：持 `wait_mu` 的生产者清 1->0，随后通知；
2. **工作线程解除等待准备**：工作线程自己重新检查后决定不睡，或从等待返回后退出该
   等待周期；
3. **强制唤醒**：关闭路径持 `wait_mu` 清状态并无条件通知。

三种 0 都已经把“这一次建立等待准备是否还需要通知”解决掉。因此后来生产者探测
读到 0 可以直接快速返回，不构成唤醒丢失。

## 9. 为什么所有修改都必须是 RMW

设多个生产者在工作线程建立等待准备前连续发布：

```text
Q1 -> P1 -> P2 -> P3 -> A
```

P1/P2/P3/A 都是 acq_rel RMW。每个 RMW 读取修改顺序的直接前驱：

```text
P1 release
  -> P2 acquire/release
     -> P3 acquire/release
        -> A acquire
```

因此发布操作的 happens-before 关系可以沿链传递给 A。

如果中间插入一个普通的存储 0 操作，后续 A 可以只 acquire 那个存储，而不是
生产者的 release 链；证明会被破坏。所以：

```text
atomic_init(..., 0)      // 发布前允许
并发生存期内使用普通存储   // 禁止
并发生存期内使用 acq_rel RMW // 必须
```

试金石测试框架在两侧线程都停在屏障时使用 relaxed 存储重置下一轮，这是
测试静止状态下的准备步骤，不是生产环境并发协议。

<a id="10-multi-producer"></a>

## 10. 多生产者

多个生产者的探测都是 `fetch_or(waiting, 0, acq_rel)`：

- waiting=0 时它们仍形成 RMW 修改顺序/release-acquire 链；
- 工作线程后续建立等待准备时，以 acquire 读取最后一项即可传递地看到先前发布操作；
- waiting=1 时一个或多个生产者可能进入慢速路径；
- 在 `wait_mu` 下只有第一个 `claim_signal()` 能从 1 清成 0 并通知；
- 后续生产者尝试取得通知职责时读到 0，直接退出。

因此 MPSC 不要求“每个生产者都通知”；只要求为当前等待准备指定一个承担处理职责的执行者。

<a id="11-condvar--spurious-wakeup"></a>

## 11. 条件变量 / 虚假唤醒

`wait_cv` 只是阻塞通知，不是状态。

工作线程始终：

```text
获取 wait_mu
while (队列为空 && running)
    建立等待准备
    重新检查
    cond_wait
解除等待准备
解锁
```

每次返回都重新检查谓词，因此虚假唤醒安全。

<a id="12-shutdown--failure"></a>

## 12. 关闭/失败

- 关闭的 `logger_queue_wake_force()` 不依赖等待提示，始终通知；
- `running=0` 后工作线程谓词循环会退出；
- `pthread_cond_wait()` 错误写入 `lifecycle_error` 并转 STOPPING；
- 最终释放队列存储仍依赖工作线程 join/静止状态，不依赖
  `consumer_waiting`。

<a id="13-aba--wraparound"></a>

## 13. ABA / 回绕

`consumer_waiting` 不是代次/身份，只是当前等待周期的交接
状态。其正确性来自：

- 单消费者；
- `wait_mu` 串行化建立等待准备 -> 重新检查 -> 睡眠/解除等待准备；
- 同一原子变量的 RMW 修改顺序；
- 生产者承担等待处理职责的规则。

队列位置/代次回绕仍由 `slot.seq` 单独处理。

<a id="14-progress-与成本"></a>

## 14. 进度与成本

整体协议不是无锁（lock-free）/无等待（wait-free）：

- 工作线程可以阻塞在条件变量；
- 生产者探测快速路径不拿互斥锁，但每个成功入队增加一个 acq_rel RMW；
- 观察到 waiting=1 的生产者可以阻塞在 `wait_mu`；
- 队列已满/溢出块耗尽仍按有界过载策略返回。

RMW 会增加 `consumer_waiting` 缓存行的写竞争。#193 已修复旧基准与
生产库配置不一致，#194 补充真实生产静态库公开 API 固定输入实测；
但二者均未提供 #116 旧双栅栏与当前 RMW 的同环境、同生产构建直接前后对照。
此成本由 [#128](https://github.com/MyBlackHole/c-logger/issues/128)
单独验收，不能把工作流绿色或历史曲线解释为“无性能退化”。
如果成本不可接受，应在 #104 重新评估唤醒原语，并重新证明完整协议。

<a id="15-c11-data-race-合法性"></a>

## 15. C11 数据竞争合法性

- `slot.seq`、`consumer_waiting`、`running` 都是原子变量；
- 负载由 `slot.seq` release/acquire 发布；
- 等待入口由 `wait_mu` 与条件变量契约串行化；
- 本协议不存在依赖 `volatile` 或普通共享对象的无同步读写。

## 16. 验证

- `queue_notify_regression`：确定性覆盖等待入口缝隙；
- `queue_wakeup_litmus`：直接竞争工作线程建立等待准备与生产者 RMW 探测，禁止
  “工作线程漏看记录 && 生产者漏看工作线程已建立等待准备”；
- `queue_mpsc_regression`：8 个生产者 + 单消费者；
- TSan：覆盖通知/MPSC/试金石测试；
- 关闭强制唤醒回归；
- 队列基准测试：评估每入队一次 acq_rel RMW 的代价。

测试是证明证据，不替代以上 C11 推理。

## 17. 规范依据

- ISO/IEC 9899:2018 5.1.2.4 / 7.17.3：原子修改顺序、
  释放序列、release/acquire synchronizes-with；
- ISO/IEC 9899:2018 7.17.7：原子读-改-写泛型操作；
- [N1570 §7.17.3 第 11 段](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)：
  同一原子对象的双 SC 栅栏约束，支持第 2 节最小模型的两个全序分支；
- N1570 §7.17.3 第 12 段：原子 RMW 读取修改顺序中紧邻前驱的值；
- WG14 DR 407 与 C++ P0668：历史背景，不能代替第 11 段的具体推导，
  不参与本项目完整 RMW 协议的正确性前提。
