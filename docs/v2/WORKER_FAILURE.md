# 工作线程失败与输出等待终止（#124）

## 范围

本协议处理工作线程在已接受请求之后进入既有 `lifecycle_error` 失败路径，
不重复 #123 的构造发布修复，不改变队列算法、公开布局或磁盘格式。

复用 sticky `lifecycle_error` 作为失败谓词，不增加 `worker_exited`、通用
completion、引用计数或等待超时。线程是否已经真正退出仍由 `pthread_join()`
证明，错误通知不能当作生命周期释放。

## 请求的各个事件

| 事件 | 当前含义 | 工作线程失败后的约束 |
|---|---|---|
| 通过入口状态检查 | 本次调用可尝试提交，不是永久健康保证 | 已进入调用可能继续，不允许提前释放其借用 |
| 预留 position | 定义队列顺序，flush 目标包含已预留位置 | 未发布队首可以形成空洞，不能伪造完成 |
| 发布 slot.seq | 负载可见，随后才返回 enqueue 成功 | 错误发生前已进入的 producer 可能在 worker 退出后发布 |
| enqueued 统计 | 入队事实，不是持久化确认 | 已发布但未输出的记录保留该统计，不谎称 emitted |
| completed_pos | 执行者已处理的连续水位，后端失败也属于已处理 | 不为唤醒 waiter 跳过空洞或推进未处理位置 |
| flush 返回成功 | 本次目标已完成且检查时无已观察失败 | 运行期失败不能冒充成功确认 |
| flush 返回生命周期错误 | 本次等待不能再保证目标，不是完成或重放 | 不保证全部在途 producer 已结束，不是可直接 free 的证明 |

显式 owner 仍必须先停止并 join 所有外部 borrower，再调用 destroy。Global 使用
既有 lifetime pin；失败的 flush 释放 pin 后 controller 才可 drain/销毁。
已经关闭入口之后新进入的 API 按既有规则返回 `ESHUTDOWN`，先前已经进入的
等待者返回原始生命周期错误；不要求两种时序返回同一种 errno。

## 协议

等待者在 `progress_mu` 下循环执行：

```text
检查 lifecycle_error -> 非零则返回原错误
检查 completed_pos   -> 达到目标则完成等待
pthread_cond_wait(progress_cv, progress_mu)
重新检查两个谓词
```

检查错误先于“水位已满足”，覆盖进入 flush 后、实际取得 progress_mu 之前已经
发生失败的情况，即使捕获的目标为零也不能把已观察到的失败报成成功。
普通 backend `first_error` 与生命周期错误保持分离：后端失败仍推进已处理水位，
并由 sticky backend error 返回，工作线程不因该错误自动退出。

工作线程沿用现有错误记录、STOPPING/running 发布及可消费前缀处理顺序。
失败退出前，在已经释放 `q.wait_mu` 之后取得 `progress_mu` 并 broadcast。
广播不修改完成计数，也不要求等待未发布的 producer 才能通知错误。
正常退出没有新增此错误广播，正常 completion 通知保持不变。

## signal-before-wait 与 wait-entry gap 证明

错误原子发布发生在最终通知之前；等待者持有 progress_mu 检查，通知者也必须
取得同一把锁。这不要求把 lifecycle_error 的所有写入迁到该 mutex 下。

- 通知者先获得并释放 mutex：之后的等待者获取同一锁，能够观察之前的错误，
  不依赖那次广播是否恰好有 waiter。
- 等待者先检查尚无错误：在它释放 mutex 前通知者无法进入广播临界区。
  `pthread_cond_wait()` 的释放 mutex 与阻塞相对于通知者获取同一 mutex 是
  原子的；因此通知不会丢在检查与真正等待的缝隙中。
- 虚假唤醒不等于完成；每次都重新检查谓词。broadcast 而不是 signal，保证
  当前所有等待者都可重新检查同一个不可恢复错误。

由 POSIX mutex 同步和条件等待契约衔接错误可见性，不以测试次数替代此推理。

参考：
- https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html
- https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_cond_broadcast.html

## 安全前提与失败边界

本协议要求已初始化、生命周期有效的 progress mutex/condition variable 能正常
同步，并假设线程最终得到调度、所等待的后端 I/O 返回。它不是硬实时或 I/O 超时
保证，也不能恢复被宿主取消的库工作线程、损坏的同步对象或无效外部生命周期。

新通知点使用 checked acquisition；获取失败时不在无所有权情况下 broadcast，
不覆盖已经锁存的原始生命周期错误。若再注入第二个独立的通知锁/条件变量故障，
本协议不宣称仍能可靠通知所有 waiter；同步 primitive 不变量失败策略由 #125
统一处理。当前正常路径的未检查 guard、通用 lockdep 检查时机也不是本修复范围。

工作线程没有增加对 producer 发布的等待。失败后队列中的未处理 slot/spill
继续由队列持有，外部 borrower 结束且工作线程成功 join 后才可由 owner 释放。
错误后的 flush 返回不保证后续记录输出，也不自动 replay 或抹掉失败证据。

## 验证

`test_worker_failure.c` 和 `run_worker_failure.sh BUILD_DIR [SANITIZER]`
链接本次 checkout 构建的生产归档，使用链接期协调，不新增运行时测试钩子：

- wait-first / error-first / wait-gap：等待已建立、错误先通知、持锁重查与睡眠缝隙。
- multiple / global：四个 waiter 均结束；Global lifetime pin 可释放并正常进入销毁。
- spurious / cancel：虚假唤醒仍重查；延迟取消在锁/借用释放后生效。
- later-slot / spill：队首空洞之后已有发布、长记录 spill 仍保持所有权。
- empty-target：已经通过入口的零水位 waiter 也观察终止错误。
- normal-gap / normal-drain / backend-error：正常发布/排空及后端失败完成水位对照。

测试中的 cond_wait EIO 为合成故障，返回时保持 mutex 所有权；不声称默认有效
POSIX 条件变量通常返回 EIO。超时仅是测试看门狗，不是产品终止机制。
`strrchr` 拦截位置必须核对为预留后的 source copy；测试同时断言 position、
slot.seq 和 enqueued，避免误把预留前阻塞当作 reservation gap。

故障测试先确认所有 waiter 返回，即使 producer 仍未发布；再放行并 join 所有
producer，最后调用 owner destroy。它不通过非法并发销毁制造或消除挂起。
错误路径断言 completed/emitted 保持零、没有虚构 drop/failed-record 计数，
成功 join 先于 instance free；正常 drain 则断言五条已接受记录全部输出。

核心 CI 在普通、ASan/UBSan、TSan 三种配置实际执行全部十三个新场景，另保留
完整 suite、startup 回归及 ABI 检查。已登记的其他问题不因这些测试通过而完成。
