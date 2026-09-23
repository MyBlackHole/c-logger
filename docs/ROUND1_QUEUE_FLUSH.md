# 缺陷收敛第一轮：队列通知、输出水位和 flush

## 状态与范围

基线是 `prod_c_logger_multi_instance_api.tar.gz`，不是前几轮的中间实验文件。
本轮修复基线审查中的 R1，以及 R2 的完成语义、错误统计和基准测试口径。
这仍是开发验证版本，不是 Production v1。审计与密码实现的发布阻断项没有因为本轮测试通过而消失。

## 队列协议

保留 bounded MPSC sequence ring 与单 consumer。
producer 在复制 record 并 release 发布 slot 后，取得 `wait_mu` 再发送通知。
consumer 在同一个 `wait_mu` 下检查队列谓词，并用 while 循环调用条件变量等待。

在“consumer 已判空、尚未实际进入等待”的窗口里，producer 可以发布 slot，但不能越过这把 mutex 提前完成通知。
consumer 开始等待、原子释放 mutex 后，producer 才取得 mutex 并 signal，避免通知丢失。
不使用定时轮询掩盖丢失唤醒，也不引入未经验证的 sleeping 标志快路径。

代价：每次成功入队的通知路径增加一次 mutex 加解锁。因此不能称整个提交路径完全 lock-free，也不宣称性能提高。
后续优化通知频率必须重新通过确定性交错测试。

`dequeue_pos` 变成原子发布的消费位置，producer 计算 HWM 不再读取并发修改的普通变量。
水位快照是近似值，限制在 `[0, queue_capacity]`；它不是排队延迟或精确最大积压的证明。
队列位置差采用无符号模运算，避免原来的有符号减法溢出，并补了跨位置回绕测试和容量溢出校验。

## 输出完成水位

增加独立于 `dequeue_pos` 的 `completed_pos`，含义是：此前的 FIFO 队列记录已经完成各自的后端输出尝试。

- worker 取走 batch 后，仍更新原有 `consumer_records`。这个旧字段明确只代表“已取出”。
- worker 格式化并执行 file/stderr/syslog 输出。
- 汇总成功/失败后，才在 `progress_mu` 下推进 `completed_pos`，并 broadcast 唤醒 flush 等待者。
- 后端失败同样推进完成水位，否则 flush 可能永远等待一个不会重试的 batch。
- 失败必须留下 sticky 错误，不能因推进水位而被当成成功。

flush 捕获 `enqueue_pos` 的固定目标，而不是等待队列变空，也不轮询 metrics 求和。
先前已预留、还没发布的 slot 也包含在目标中；后续请求不能越过它完成这个水位。
目标捕获后进入的并发日志不要求全部包含；同步日志中在 flush 调用前已成功返回的输出同样会被随后的文件同步覆盖。

锁顺序：队列等待 mutex 不跨格式化/I/O 持有；worker 在释放 emit mutex 后才更新 progress；flush 在等待 progress 时不持有 emit mutex。
因此 flush 不会拿着 worker 需要的 emit mutex 等待 worker 自己完成。

## 公共接口

新增 POSIX 风格接口：

```c
int logger_flush_instance_status(logger_t *logger);
int logger_flush_status(void);
```

成功返回 0；失败返回 -1 并设置 errno。
flush 先等待目标输出水位，再同步文件后端。文件创建或轮换后有待同步的目录项也会检查同步结果。
轮换旧文件的同步失败现在会向输出路径传播，不再忽略后继续 rename。

已有 `logger_flush_instance()`、`logger_flush()` 保留 void 签名，但使用同一条等待路径并丢弃返回状态。
需要知道日志是否可靠完成的代码必须用 status 接口，不能再依赖 void 接口判断成功。
`audit_flush()` 也改为传播底层 status，但这不等于修好了 Audit 自身的提交/恢复状态机。

```c
LOGGER_INFO(log, "storage", "write completed id=%u", id);
if (logger_flush_instance_status(log) != 0) {
    int saved = errno;
    /* 使用独立错误通道处理 saved；不要递归依赖失效的同一个 logger。 */
}
```

边界：

- 文件 fsync 与本地发送完成不等于远端 syslog 已持久化。
- 不是跨多个后端的原子事务；后端之一失败时另一个可能已经写入。
- 同步回退可能越过排队中的普通记录；本轮没有承诺所有同步/异步路径的全局总序。
- 等待是阻塞的，不包含内核 I/O 超时/取消能力；syslog 阻塞问题仍在待修清单。
- 显式指针必须在整个调用期间有效，owner 要在 destroy 之前停止并 join 所有使用者。
- 本轮没有扩大 fork+continue 的支持范围，继承实例不得当作可安全继续工作的运行时。

## 错误和指标

保留 `logger_metrics_t` 原有结构布局；新增独立 `logger_io_metrics_t` 与实例/全局 getter。

| 字段 | 含义 |
| --- | --- |
| `enqueued` | 成功进入异步队列的记录 |
| `consumer_records` | 出队的记录；不代表写出 |
| `dropped[]` | 队列满且采用 DROP 的记录 |
| `sync_fallbacks` | 队列满后选择同步回退的尝试；不再同时计入 dropped |
| `async_completed` | 异步输出尝试已结束的记录，包括成功和失败 |
| `sync_completed` | 同步/同步回退输出尝试已结束的记录 |
| `emitted_records` | 所选后端操作均返回成功的记录，不等于永久存储证明 |
| `failed_records` | 无法确认所有后端成功的记录 |
| `first_error` | 当前实例首次输出/同步错误的正 errno；实例内不自动清除 |

多记录 writev 在短写后失败时，可能已有部分字节到达文件。本轮保守地将整个未获完整确认的 batch 归入 failed，绝不把它声称为“这些记录都没写过”。
仅 flush 的 fsync 失败不会凭空增加一个 failed record，而会设置 first_error 并返回失败。
之后一次成功写入或一次成功 fsync 不会抹掉更早的错误。普通 logger 仍可 best-effort 继续输出，但 status flush 持续报错。
需要新的无错误生命周期时，由 owner 完成关闭/排查后创建新实例；本轮没有添加静默清除错误接口。

实时 metrics 是跨多个原子计数的近似快照。停止 producer 并完成屏障后，可检查：

```text
async_completed + sync_completed = emitted_records + failed_records
```

对本轮 benchmark 的固定 INFO 异步输入，额外检查：

```text
attempted = enqueued + sync_fallbacks + dropped
async_completed = enqueued
sync_completed = sync_fallbacks
```

## Benchmark 修正

不再通过 `consumer_records + sync_fallbacks + dropped` 轮询完成。
内部 benchmark 使用真正输出水位等待，再计算 `emitted_records / elapsed`。
`/dev/null` 没有文件持久化契约，benchmark 的等待不调用 fsync；输出标记明确为 `output_completion_not_durability`。
它只是管线 smoke test，不是新性能收益结论。历史吞吐/formatter 对比因旧完成语义和已知丢失唤醒问题不作为发布基线。

## 测试与验收

本轮新增 16 个 CTest 场景：两个基线回归、固定水位/多个等待者、MPSC 精确记录和位置回绕、global flush、10 个 I/O/回退情形，以及 benchmark 计数 smoke。
确定性交错和系统调用注入通过测试可执行文件的 GNU linker `--wrap` 完成，没有为这些场景新增生产回调。
已有环境变量 fault/crash hook 仍是另一个需要修复的发布阻断项，不能将二者混为一谈。

同时修复验收工程中直接影响可信度的三点：CTest labels 使用追加语义，sanitizer 环境设置不覆盖 fault 环境，check 依赖测试可执行文件。
每个 CTest case 使用独立 working directory，新增场景进一步使用 mkdtemp，支持本轮已执行的并行和重复测试。

详见 `validation/RESULTS.md` 及原始日志。仅专项 sanitizer 通过，不代表整个库 sanitizer 清洁；单独运行 SM3 仍会触发基线里的 UB。
