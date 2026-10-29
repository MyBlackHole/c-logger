# c-logger 2.0 锁与阻塞契约

状态：**#103 第一阶段设计基线**。

本文件定义不含 Audit 的 ABI 2 当前八种锁类的锁图；Audit 控制器/操作已退役。修改锁图、增加嵌套或改变条件谓词时必须同步更新。

## 1. pthread 锁矩阵

| 锁 | 保护对象/职责 | 允许前置锁 | 后续可取锁 | 生命周期作用 | 阻塞上下文 |
|---|---|---|---|---|---|
| Global `g_control_mu` | 初始化/关闭控制器 | 无 | `g_lifetime_lock`，随后实例锁 | 控制器独占性，不直接固定保护对象生命周期 | MAY_BLOCK |
| Global `g_lifetime_lock` | `g_logger` 发布/撤销发布 + 读者生命周期固定保护（pin） | 写者可持 `g_control_mu`；读者无 | 实例锁 | **是：当前全局读者生命周期固定保护（pin）** | MAY_BLOCK |
| `logger_t.emit_mu` | 文件/syslog 后端可变状态、写入/同步/重新打开 | 全局生命周期固定保护（pin） / 显式调用方作用域 | 不允许进度/等待锁 | 否 | MAY_BLOCK |
| `logger_t.progress_mu` | `completed_pos` + `progress_cv` 谓词 | 全局生命周期固定保护（pin） / 显式调用方作用域 | 不允许输出/等待锁 | 否 | MAY_BLOCK |
| `struct logger_queue.wait_mu` | 工作线程睡眠谓词 + 条件变量握手 | 无 | 不允许输出/进度锁 | 否 | MAY_BLOCK，仅慢速路径 |
| Console `g_console_mu` | FILE 输出 + 刷新 | 无 | 格式协调锁 | 否 | MAY_BLOCK |
| 运行域 `format_mu` | 本库格式化/时间/标准流的 fork 一致边界 | Global 控制/生命周期、实例输出、Console 或无 | 资源协调锁 | 否 | MAY_BLOCK |
| 运行域 `resource_mu` | fd/映射候选的取得、登记、移交与消费 | Global 控制/生命周期、实例输出、格式协调或无 | 不允许继续拿锁 | 否；不替代对象静默证明 | MAY_BLOCK |

<a id="2-非-pthread-lock-同步机制"></a>

## 2. 非 pthread 锁的同步机制

以下不能混进互斥锁层级：

- 文件 `<active>.logger.lock`：跨进程所有权；
- Global `g_ticket`：阶段 + 代次准入；
- 队列 `slot.seq`：负载发布 + 复用代次；
- 队列 `spill_used[]`：溢出块所有权/复用；
- `running`：工作线程停止状态；
- `state`：实例准入状态；
- 进程 `live_objects`：存活对象计数；
- 引用计数：独立所有者生命周期（当前无生产环境对象使用）。

<a id="3-instance-lock-禁止嵌套"></a>

## 3. 实例锁禁止嵌套

当前禁止：

```text
emit_mu -> progress_mu
progress_mu -> emit_mu
emit_mu -> q.wait_mu
q.wait_mu -> emit_mu
progress_mu -> q.wait_mu
q.wait_mu -> progress_mu
```

工作线程的流程必须拆成：队列消费 -> 输出 -> 进度更新，而不是同时持多把实例锁。

<a id="4-global-lock-order"></a>

## 4. Global 锁顺序

控制器有两种合法阶段：

```text
已发布/全局对象：
g_control_mu
    -> g_lifetime_lock（写锁）
        -> 实例操作

控制器拥有的候选/退役对象：
g_control_mu
    -> 实例操作
```

第二条只适用于对象尚未发布，或已经在生命周期写锁下撤销发布并完成
读者排空之后。此时对象所有权已独占转回控制器，生命周期固定保护（pin）不再
承担该对象的可达性证明。

即使允许 `g_control_mu -> instance`，仍禁止：

```text
g_control_mu -> instance -> g_lifetime_lock
```

所以不能先拿实例锁，再回头进入生命周期锁图。

读者：

```text
g_lifetime_lock（读锁）
    -> 实例操作
```

禁止反向：

```text
instance lock -> g_lifetime_lock
instance lock -> g_control_mu
g_lifetime_lock -> g_control_mu
```

<a id="5-condition-variable-contract"></a>

## 5. 条件变量契约

`progress_cv` 必须与 `progress_mu` 配对；谓词是完成水位已达到，
或已锁存的 `lifecycle_error` 已发布。工作线程失败退出前在 `progress_mu` 下广播；
等待者必须在同一锁下检查错误，不能等待已不存在的执行者。失败不伪造完成水位，
也不解除对象借用。完整边界及双向竞争证明见 `WORKER_FAILURE.md`。

`q.wait_cv` 必须与 `q.wait_mu` 配对；谓词是队列状态与 running 状态。

所有等待都必须：

```text
获取 mutex
while (!predicate)
    cond_wait
释放 mutex
```

通知/广播只是通知，不是状态本身。

## 6. guard 使用边界

简单单锁词法作用域可以使用 `guard/ACQUIRE`。

以下保留显式控制流：

- join -> destroy -> free 的销毁流程；
- Global 控制/生命周期多锁；
- 恢复取消与锁释放顺序本身属于契约的路径。

## 7. lockdep 检查与实现边界

不能通过对未包装的普通 `pthread_mutex_t` 调 `pthread_mutex_trylock()` 来假装实现 `lockdep_assert_held()`：

- 无法可靠区分“当前线程持有”和“其他线程持有”；
- 普通互斥锁的自身尝试加锁/所有权语义不是内核 lockdep；
- 可能改变程序状态或把调试辅助函数变成同步操作。

当前实现使用**显式调试所有权跟踪**，仅在测试/调试构建记录：

```text
锁类别
锁对象地址
线程本地持锁栈与深度
获取顺序
```

可能阻塞的受跟踪锁在 pthread 获取前执行无状态锁序检查，成功后登记；trylock
先执行 pthread 操作，仅成功时检查并登记。解锁在 pthread 操作前验证当前线程持锁
顺序，只有解锁成功后才移除记账。`pthread_cond_wait()` 返回时互斥锁已重新获取，TLS
记账在等待期间保持不变。

生产构建通过 `LOGGER_ENABLE_LOCKDEP=0` 将检查与记账编译为空操作。此机制使用固定锁
类别关系，不是完整 Linux lockdep，也不观察未接入包装的 pthread 锁。跟踪时机和覆盖
范围详见 `docs/v2/LOCKDEP.md`。

lockdep 仅接入已建模的锁类别，不要求把项目所有 pthread 互斥锁替换成新的通用锁抽象。
未接入跟踪包装的 pthread 锁仍遵守其原有调用契约。

<a id="8-callbackreentry"></a>

## 8. 回调/重入

持有以下锁时禁止调用未知宿主回调：

- Global 控制/生命周期锁；
- Logger 实例锁；
- Console 输出锁。

Logger/Console 当前通过 TLS 运行时作用域拒绝同线程直接重入；这不是跨线程死锁证明，也不是信号处理器安全保证。

<a id="9-forkcancellation"></a>

## 9. fork/取消

fork 子进程不允许接触继承来的 pthread 锁状态；进程身份门禁必须先执行。
有效快照下，子方显式初始化建立自己的进程域，旧句柄不恢复有效性。
父方 prepare 按 `format_mu -> resource_mu` 固定边界，parent 逆序释放；
child 只处理继承描述符和当前线程标记，不解锁 DONTFORK 地址。

资源协调锁不得跨越等待工作线程退出、条件等待或需要回到格式锁的代码。
临时 DIR 的资源租约必须由创建它的线程消费。fork 期间同线程回调不得重入
普通 Logger；库内格式协调不能保护宿主直接进入的 libc 隐含锁。

格式主操作和解锁错误独立处理：主操作失败保留其错误；主操作成功但解锁失败
仍报告失败并封闭域。普通输出只允许容量不足的显式截断，不能把锁/编码错误
吞为成功。批量格式预处理一旦失败，在任何后端输出前拒绝整批并记失败，
仍推进已处理批次的完成水位，不声称已持久化。

取消恢复必须发生在全部库锁/生命周期固定保护（pin）释放之后。

`pthread_cond_wait()` 返回时互斥锁已重新获取，清理/guard 才能负责最终解锁。

<a id="10-锁变更-review"></a>

## 10. 锁变更审查

新增或修改锁必须说明：

1. 保护什么；
2. 执行上下文；
3. 允许的前置锁；
4. 允许的后续锁；
5. 是否参与生命周期证明；
6. 是否跨回调/I/O；
7. 条件谓词；
8. 取消/fork 影响；
9. 是否需要 lockdep 风格的断言；
10. 失败路径测试。
