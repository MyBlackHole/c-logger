# 锁与锁顺序规范

锁只负责保护不变量，不自动提供对象生命周期。每一把锁都必须明确：

1. 保护哪些字段或状态；
2. 谁可以获取；
3. 与其他锁的固定顺序；
4. 是否允许等待、I/O、回调或重入；
5. 是否适合用 `guard()/ACQUIRE()` 自动释放。

完整清单见 `LOCK_MATRIX.md`。

<a id="global-lock-hierarchy"></a>

## Global 锁层级

全局 Logger 的固定顺序是：

```text
g_control_mu
    ->
g_lifetime_lock
    ->
Logger 实例锁
```

禁止在持有全局生命周期读侧固定保护（pin）或实例锁时反向获取
`g_control_mu`。

职责：

- `g_control_mu`：串行化 初始化/关闭等生命周期控制操作；
- `g_lifetime_lock`：在读侧借用者使用 `g_logger` 时阻止发布/销毁；
- 实例锁：保护单个 `logger_t` 内部状态。

代次令牌只是准入/版本机制，不能替代生命周期锁。

<a id="logger-instance-locks"></a>

## Logger 实例锁

### emit_mu

`emit_mu` 串行化后端可变状态和同步输出。

主要保护：

- `file_backend` 的活动文件 fd / 轮转 / 重新打开状态；
- `syslog_backend` 的连接与指标；
- 后端写入/同步/重新打开的顺序。

禁止在持有 `emit_mu` 时等待工作线程进度。

### progress_mu

`progress_mu` 只保护异步完成的条件变量协议：

- `completed_pos`；
- `progress_cv` 的等待/广播。

原子完成计数器可按自己的内存序独立读取，但不能把
`completed_pos` 当作原子谓词的替代品。

<a id="spill-bitmap"></a>

### 溢出块位图

长消息溢出块不再由互斥锁保护的空闲链表管理。`spill_used[]` 是无锁（lock-free）原子
所有权位图，不属于互斥锁层级。

它只表示块是否被独占，不保护队列负载发布；正文可见性仍由
`slot.seq` release/acquire 建立。

### q.wait_mu

`q.wait_mu` 只负责工作线程在队列为空时的睡眠/唤醒协议。

它不保护 MPSC 负载发布。生产者通过槽位序列原子变量发布。
工作线程在持锁状态通过 acq_rel exchange 建立等待准备 `consumer_waiting=1`，然后重新检查队列；
普通生产者在槽位发布后执行 acq_rel `fetch_or(waiting, 0)` RMW 探测，仅取得旧值 1
才进入这把锁，通过 acq_rel exchange 取得通知职责后发出通知。因此有积压/消费者活跃场景
不再对每条日志加锁/通知，但 RMW 探测不能替换为普通加载。

关闭的强制唤醒始终获取 `q.wait_mu`，通过 acq_rel RMW 解除等待准备后无条件发出通知。
完整交接与条件变量证明见 [QUEUE_WAKEUP_PROOF.md](v2/QUEUE_WAKEUP_PROOF.md)。

<a id="instance-lock-之间的关系"></a>

### 实例锁之间的关系

当前 `emit_mu`、`progress_mu`、`q.wait_mu` **不允许相互嵌套**。

例如 flush 必须：

```text
等待 progress_mu 条件
    ->
释放 progress_mu
    ->
获取 emit_mu
    ->
同步后端
```

不能为了减少加锁/解锁次数把两个阶段合并成嵌套持锁。

<a id="console-lock"></a>

## Console 锁

`g_console_mu` 只串行化 Console 的 FILE 输出与 flush。

`g_config` 使用原子快照，不依赖 `g_console_mu`。

Console 锁与 Logger 锁没有允许的嵌套关系；不要在持有该锁时调用可能重入
Logger/Console 的外部回调。

## guard 使用规则

### 适合 guard

满足以下条件时优先使用：

- 单锁；
- 锁持有期与词法作用域一致；
- 解锁错误原本就不承担业务返回语义；
- 不需要跨作用域转移锁所有权；
- 不涉及复杂取消清理。

示例：

```c
{
    guard(pthread_mutex)(&l->emit_mu);
    update_backend();
}
```

<a id="需要检查-lock-error"></a>

### 需要检查加锁错误

如果原代码检查 `pthread_mutex_lock()` 返回值，必须使用：

```c
ACQUIRE(pthread_mutex_checked, lock)(&mu);
int rc = ACQUIRE_ERR(pthread_mutex_checked, &lock);
if (rc)
    return rc;
```

不能为了使用 guard 而吞掉 pthread 错误。

### 不适合 guard

以下情况保持显式锁管理，直到整个协议一起重构：

- 全局多锁层级；
- 解锁错误会参与返回值；
- 锁所有权需要跨词法作用域；
- pthread 取消处理器需要显式参与；
- 锁/解锁中间存在特殊发布/回滚阶段。

<a id="lock-scope"></a>

## 锁作用域

优先使用最小词法作用域：

```c
{
    guard(pthread_mutex)(&lock);
    mutate_protected_state();
}
/* 独立慢路径放到锁外 */
```

不要因为 guard 写起来方便，就把格式化、无关 I/O、等待或回调扩大到锁内。

<a id="locking-与-lifetime-的区别"></a>

## 加锁与生命周期的区别

两者回答的问题不同：

- 锁：多个使用者能否同时访问某个不变量；
- 生命周期固定保护（pin）/ref/join：对象在使用期间会不会消失。

在锁内拿到一个指针，并不代表解锁后指针仍然有效；
解锁后能否继续使用必须由生命周期协议单独证明。

<a id="lock-order-review"></a>

## 锁顺序审查

新增锁获取前必须检查：

1. 保护的数据/不变量；
2. 调用方是否已经持有其他锁；
3. 固定层级；
4. 被调函数会不会再拿锁；
5. 回调是否可能重入；
6. 临界区是否睡眠或执行无界 I/O；
7. 清理/析构函数执行时是否仍依赖该锁；
8. 是否真的适合词法作用域 guard。

复杂多锁函数在完整锁图没有明确之前保持显式。
