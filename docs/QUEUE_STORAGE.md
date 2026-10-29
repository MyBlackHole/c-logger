<a id="queue-compact-storage-与-long-message-spill-pool"></a>

# 队列紧凑存储与长消息溢出块池

## 目标

旧源码的类型名在历史比较处保留；当前 `struct logger_message` / `struct logger_queue`
与旧名称的对照见 [编码规范](v2/CODING_STYLE.md)，历史基准数据与验证身份不改写。

旧队列槽位直接内嵌完整 `logger_message_t`，其中仅正文就包含
`text[4096]`。队列容量较大时，即使绝大多数日志只有几十到几百字节，也会为每个
槽位固定支付 4KiB 正文空间。

本轮把队列存储拆成：

```text
紧凑槽位
    = 元数据
    + 上下文快照
    + 源信息快照
    + 512B 内嵌正文
    + 可选溢出块索引

预分配溢出块池
    = 最多 1024 × LOGGER_MESSAGE_MAX
```

公开 API、格式化结果和同步路径仍使用完整 `struct logger_message`。

<a id="inline-与-spill-边界"></a>

## 内嵌与溢出块边界

- 正文长度 <= 512B：直接保存在 `inline_text`；
- 正文长度 > 512B：从溢出块池取得一个块；
- 每个溢出块保留完整 `LOGGER_MESSAGE_MAX` 空间；
- 队列容量 < 1024 时，溢出块数量等于队列容量；
- 队列容量 >= 1024 时，溢出块数量固定上限为 1024。

因此，大队列的长消息容量与槽位容量是刻意解耦的。

例如默认队列容量 8192：

```text
短消息容量 = 8192 个槽位
长消息排队容量 <= 1024 个溢出块
```

这是内存上界换取的明确设计，不应被描述成“所有消息都拥有 8192 的有效排队容量”。

<a id="spill-exhaustion"></a>

## 溢出块耗尽

溢出块池用尽时绝不静默截断长消息。

`logger_queue_push()` 返回不可用，上层继续执行已有按级别配置的溢出策略：

```text
TRACE/DEBUG/INFO/WARN 默认 -> DROP
ERROR/FATAL 默认          -> SYNC 回退
```

因此：

- 不增加静默截断；
- 不在热路径临时 malloc；
- 长消息突发超出预分配上限时行为可预测；
- 基准测试会单独报告 `queue_spill_exhaustions`。

<a id="ownership"></a>

## 所有权

<a id="queue-lifetime"></a>

### 队列生命周期

`struct logger_queue` 结构性拥有：

- 紧凑 `slots`；
- `spills`；
- 无锁（lock-free） `spill_used[]` 位图；
- `consumer_waiting + wait_mu/wait_cv` 自节奏唤醒状态。

最终都由 `logger_queue_destroy()` 在工作线程 join 后释放/销毁。

<a id="单个-spill-block"></a>

### 单个溢出块

单个块的生命周期：

```text
队列空闲位图
    ->
生产者通过 CAS 认领后临时独占
    ->
生产者写正文
    ->
槽位以 release 发布溢出块索引
    ->
消费者以 acquire 观察槽位
    ->
消费者复制正文到工作线程批次
    ->
消费者通过原子清零归还位图中的块
```

生产者在队列已满时会在返回前把块归还，因此不会泄漏。

不使用引用计数：块的所有权在生产者 -> 已发布槽位/消费者 -> 池
之间是严格移动/独占关系。

<a id="concurrency"></a>

## 并发

`spill_used[]` 是固定大小原子位图，不使用共享互斥锁/空闲链表。

生产者扫描位图字，通过 0->1 CAS 独占块；消费者完成正文复制后，
通过原子清零把位从 1->0。每个线程从自己的 TLS 探测起点开始扫描，避免所有
生产者从同一个字开始竞争。

正文 memcpy 发生在块认领成功之后，不需要持锁。

槽位发布仍沿用原 MPSC release/acquire：

```text
生产者写溢出正文 + 紧凑记录
    ->
以 release 存储 slot.seq

消费者以 acquire 加载 slot.seq
    ->
读取紧凑记录 / 溢出正文
```

消费者必须先完成正文复制并归还溢出块，再把 slot.seq 标成可复用。

`spill_used[]` 不属于互斥锁层级。它只承担块所有权状态；
槽位发布仍由 `slot.seq` 的 release/acquire 保证。

<a id="demand-driven-metadata-snapshot"></a>

## 按需采集元数据快照

紧凑槽位仍保留固定元数据存储，因此队列的内存上界和源信息截断
语义不变：

- 模块存储 128B；
- 文件名存储 256B；
- 函数名存储 128B；
- request/session/trace 各 64B。

但生产者不再无条件填充这些区域。实例创建时根据不可变的
`detail/include_*` 生成私有 `capture_mask`：

```text
MINIMAL
    -> 不采集 module/pid/tid/context/source

NORMAL
    -> module

VERBOSE
    -> module + 已配置的 pid/tid

DEBUG
    -> module + 已配置的 pid/tid + context
       + 已配置的 source
```

PID 在 Logger 创建时缓存；原始 fork 子进程本来就会在访问继承运行时前被拒绝，
因此父 Logger 生命周期内无需每条日志重新 `getpid()`。

异步队列只为掩码指定的 module/context/source 采集快照；不用的字段只把首字节/长度清零，
不再固定复制约 700B 元数据。真正需要的源信息仍使用原有界复制，所以
DSO/源信息生命周期契约不变。

<a id="worker-workspace"></a>

## 工作线程工作区

工作线程批次仍使用完整 `struct logger_message`，但容量最大为 256。

这是有意保留的第二阶段结构：

- 队列承担大规模待处理记录存储，因此优先采用紧凑表示；
- 工作线程批次只保存一个有限批次，继续使用完整消息结构 可保持格式化/后端路径简单；
- 后续如果基准测试证明工作区仍值得优化，可单独设计，不与队列发布
  同时变化。

<a id="benchmark"></a>

## 基准测试

`bench_matrix` 现在报告：

- `queue_slot_bytes`
- `queue_slot_storage_bytes`
- `queue_spill_capacity`
- `queue_spill_storage_bytes`
- `queue_spill_exhaustions`
- `queue_storage_bytes`

`queue_storage_bytes` 只统计队列的预分配槽位 + 溢出块存储，不包含
`logger_t`、pthread 对象和工作线程工作区。

基准测试矩阵继续覆盖 64 / 256 / 1024 / 4000B 消息，长消息压力下必须同时观察
吞吐、丢弃/同步回退与溢出块耗尽，不能只比较 logs/sec。

<a id="hot-path-优化"></a>

## 热路径优化

首次紧凑基准测试暴露出两个不同热点：

1. 256B 正文过早进入溢出块路径，4/16 生产者出现明显溢出块耗尽；
2. 4KiB 单生产者即使没有溢出块耗尽也明显变慢，说明不能只归因于池容量。

因此当前实现做三项针对性优化：

- 内嵌最大正文从 255B 提升到 **512B**；
- 溢出块空闲链表互斥锁改为无锁（lock-free）原子位图；
- `struct logger_message.text_len` 直接保存 `vsnprintf()` 已知的实际写入长度，
  异步入队不再重新扫描最多 4095B 正文。

源信息快照同时保存实际长度，减少固定尾部内存流量。

这些优化不扩大 1024 个溢出块上限，先解决热路径，再根据同一运行器上的基准测试
判断是否真的需要调整容量。


<a id="beforeafter-benchmark-验证"></a>

## 前后版本基准验证

基准测试工作流现在同时构建三个版本：

```text
内存基线：
23504f104e900419e891b57d7187ca0bcabffdf3
    = 紧凑存储改造前，仅用于 >=50% 内存门禁

吞吐调优基线：
6b926d59cda3bf176baa10b24e2c84bc49ce7943
    = 256B 内嵌 + spill_mu 的上一版紧凑存储

候选版本：
当前提交
```

三者在同一个 GitHub Actions 运行器构建，但不能仅凭 Release 名称认定配置等价。
历史基线沿用各自 CMake 描述；当前候选由 Xmake 直接链接生产静态 `logger`，
库与基准驱动均优化，锁诊断/故障注入关闭。此前候选曾误链未优化的回归支持库，
旧新数据不能混用。工作流明确传入 `--build-configs-differ`，保留原始吞吐值但不计算
比值，不将差异归因于单项源码优化。私有观测与共享 DSO 边界见源码树中的
`BENCHMARK.md`（不随生产包安装）。

对比矩阵：

```text
threads = 1 / 4 / 16
message = 64 / 256 / 1024 / 4000 B
每线程 1000 条记录
每个组合重复 3 次，取中位数
```

### 确定性门禁

吞吐受共享运行器调度、CPU 频率、虚拟化噪声影响，因此不把 logs/sec 比值
作为 CI 通过/失败条件。

硬门禁继续以紧凑存储改造前的布局为内存基线：

```text
默认队列容量 = 8192
候选版本队列存储相对紧凑存储改造前至少下降 50%
```

<a id="hotspot-对比"></a>

### 热点对比

历史报告以“上一版紧凑存储”为比较对象；当前工作流因构建口径不同不计算吞吐比值，
也不据此宣称 512B 内嵌、位图分配器或 text_len/源信息长度优化的独立收益。

在另行证明构建口径等价的比较中，仍要求调优基线/候选版本都没有丢弃、同步回退、
溢出块耗尽才计算比值；否则标记为 `overload`，避免把少处理日志误解释成性能提升。

工作流输出完整原始样本、Markdown 摘要和 Actions 制品。实测结论写入
`validation/QUEUE_HOTPATH.md`。


<a id="self-paced-worker-wakeup"></a>

## 工作线程自节奏唤醒

普通入队不再无条件执行 `pthread_mutex_lock + pthread_cond_signal`。

当前协议在同一个 `consumer_waiting` 原子变量上使用 acq_rel RMW 交接，已移除旧双 SC 栅栏协议。
初始化后，只要生产者/工作线程/强制唤醒仍可能并发，所有 waiting 修改都必须经过这些 RMW 辅助函数。

工作线程在队列空时：

```text
获取 wait_mu
  -> exchange(consumer_waiting, 1, acq_rel)  // 准备等待
  -> 重新检查队列/running
  -> 仍为空/仍运行 ? cond_wait : 不睡眠
  -> exchange(consumer_waiting, 0, acq_rel)  // 结束等待
  -> 释放 wait_mu
```

生产者在记录的 `slot.seq` release 发布后执行
`fetch_or(consumer_waiting, 0, acq_rel)` 探测；虽然数值不变，它仍是 RMW，不能替换成普通加载：

- 返回 0：不获取 `wait_mu`；该 RMW 的发布链仍可传给后续工作线程建立等待准备；
- 返回 1：进入 `wait_mu` 慢速路径，执行 `exchange(consumer_waiting, 0, acq_rel)`；
  只有取得旧值 1 的生产者才发出通知，旧值 0 表示该等待周期已被其他执行者处理。

关闭/停止先关闭 `running`，再持 `wait_mu` 通过 acq_rel RMW 解除等待准备并无条件强制通知，
不依赖生产者的 waiting 探测。

这里的发布链必须包括当前出队队首的生产者；后续槽位先发布不保证
队首已可消费。填补队首预留空洞的生产者仍须在发布后执行自己的 RMW 探测。
完整 C11 happens-before、MPSC 链、条件变量等待入口和等待处理责任证明见
[QUEUE_WAKEUP_PROOF.md](v2/QUEUE_WAKEUP_PROOF.md)。试金石测试、等待入口回归与 TSan 是证据，不能替代该证明。

基准测试私有字段同时报告 `queue_wait_count`、`queue_producer_wake_signals` 和
`queue_force_wake_signals`。上一版实现每次成功入队都通知，因此调优
基线的生产者通知数等于 enqueued；候选版本可以直接计算通知次数减少比例。

RMW 交接是正确性协议的一部分；不能用 普通加载/存储或添加栅栏来替代。
任何替换都必须重新证明唤醒丢失不可能，并保留上述验证边界。
