# 锁与锁顺序规范

锁只负责保护 invariant，不自动提供对象 lifetime。每一把锁都必须明确：

1. 保护哪些字段或状态；
2. 谁可以获取；
3. 与其他锁的固定顺序；
4. 是否允许等待、I/O、callback 或 reentry；
5. 是否适合用 `guard()/ACQUIRE()` 自动释放。

完整清单见 `LOCK_MATRIX.md`。

## Global lock hierarchy

global logger 的固定顺序是：

```text
g_control_mu
    ->
g_lifetime_lock
    ->
logger instance locks
```

禁止在持有 global lifetime read pin 或 instance lock 时反向获取
`g_control_mu`。

职责：

- `g_control_mu`：串行化 init/shutdown 等 lifecycle controller；
- `g_lifetime_lock`：在 read-side borrower 使用 `g_logger` 时阻止 publish/destroy；
- instance locks：保护单个 `logger_t` 内部状态。

generation ticket 只是 admission/version 机制，不能替代 lifetime lock。

## Logger instance locks

### emit_mu

`emit_mu` 串行化 backend 可变状态和同步输出。

主要保护：

- `file_backend` 的 active fd / rotation / reopen 状态；
- `syslog_backend` 的 connection 与 metrics；
- backend write/sync/reopen 的顺序。

禁止在持有 `emit_mu` 时等待 worker progress。

### progress_mu

`progress_mu` 只保护 async completion 的 condition-variable 协议：

- `completed_pos`；
- `progress_cv` 的 wait/broadcast。

atomic completion counter 可按自己的 memory order 独立读取，但不能把
`completed_pos` 当作 atomic predicate 的替代品。

### q.spill_mu

`q.spill_mu` 只保护 long-message spill freelist。

producer 取得 block 后立即释放 `spill_mu`，正文 memcpy 在锁外完成；consumer 把正文复制到
worker batch 后，再短暂获取 `spill_mu` 归还 block。

它不保护 queue slot、payload publication 或 worker completion，也不能与
`q.wait_mu/emit_mu/progress_mu` 嵌套。

### q.wait_mu

`q.wait_mu` 只负责 worker empty-queue 的 sleep/wakeup 协议。

它不保护 MPSC payload publication。producer 通过 slot sequence atomic publish；
`q.wait_mu` 只防止 consumer 检查 empty 与进入 `pthread_cond_wait()` 之间丢 wakeup。

### instance lock 之间的关系

当前 `emit_mu`、`progress_mu`、`q.spill_mu`、`q.wait_mu` **不允许相互嵌套**。

例如 flush 必须：

```text
等待 progress_mu 条件
    ->
释放 progress_mu
    ->
获取 emit_mu
    ->
sync backend
```

不能为了减少 lock/unlock 次数把两个阶段合并成嵌套持锁。

## Console lock

`g_console_mu` 只串行化 Console 的 FILE 输出与 flush。

`g_config` 使用 atomic snapshot，不依赖 `g_console_mu`。

Console lock 与 Logger/Audit 锁没有允许的嵌套关系；不要在持有该锁时调用可能重入
Logger/Console 的外部 callback。

## Audit locking

Audit 使用两级 in-process 锁：

```text
g_control_mu
    ->
g_operation_mu
    ->
Audit private logger instance locks
```

- Audit `g_control_mu`：init/shutdown/controller；
- Audit `g_operation_mu`：已 publish runtime、sequence/hash、transaction 操作；
- 持有 operation lock 的 Audit 路径可能调用 private logger，因此 instance lock 位于其后。

Audit 还存在 persistent writer-lock fd。它是跨进程 ownership/exclusivity 机制，
**不是 pthread lock**，不能代替进程内 mutex。

Audit lock 路径还包含 cancellation policy，因此本轮不机械迁移到 guard。

## guard 使用规则

### 适合 guard

满足以下条件时优先使用：

- 单锁；
- lock lifetime 与 lexical scope 一致；
- unlock error 原本就不承担业务返回语义；
- 不需要跨 scope transfer 锁 ownership；
- 不涉及复杂 cancellation cleanup。

示例：

```c
{
    guard(pthread_mutex)(&l->emit_mu);
    update_backend();
}
```

### 需要检查 lock error

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

- global 多锁 hierarchy；
- Audit control/operation 多锁；
- unlock error 会参与返回值；
- 锁 ownership 需要跨 lexical scope；
- pthread cancellation handler 需要显式参与；
- lock/unlock 中间存在特殊 publish/rollback 阶段。

## Lock scope

优先使用最小 lexical scope：

```c
{
    guard(pthread_mutex)(&lock);
    mutate_protected_state();
}
/* 独立慢路径放到锁外 */
```

不要因为 guard 写起来方便，就把格式化、无关 I/O、等待或 callback 扩大到锁内。

## locking 与 lifetime 的区别

两者回答的问题不同：

- lock：多个使用者能否同时访问某个 invariant；
- lifetime pin/ref/join：对象在使用期间会不会消失。

在锁内拿到一个 pointer，并不代表 unlock 后 pointer 仍然有效；
unlock 后能否继续使用必须由 lifetime protocol 单独证明。

## lock-order review

新增 lock acquisition 前必须检查：

1. 保护的 data/invariant；
2. caller 是否已经持有其他锁；
3. 固定 hierarchy；
4. 被调函数会不会再拿锁；
5. callback 是否可能重入；
6. 临界区是否 sleep 或执行无界 I/O；
7. cleanup/destructor 执行时是否仍依赖该锁；
8. 是否真的适合 lexical guard。

复杂多锁函数在完整 lock graph 没有明确之前保持显式。
