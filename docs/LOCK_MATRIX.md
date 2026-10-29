<a id="lock-matrix"></a>

# 锁矩阵

本表记录 c-logger 当前主要同步锁的职责、顺序与 guard 策略。修改锁图时必须同步更新。

| 锁 | 生命周期 | 保护对象/职责 | 允许的前置锁 | 是否允许继续拿锁 | guard 策略 |
|---|---|---|---|---|---|
| 全局 `g_control_mu` | 进程运行域 | 全局初始化/关闭控制器 | 无 | `g_lifetime_lock`，随后实例锁 | 保持显式 |
| 全局 `g_lifetime_lock` | 进程运行域 | `g_logger` 发布/销毁与读侧生命周期固定保护（pin） | `g_control_mu`（写者/控制器）或无（读者） | 实例锁 | 保持显式 |
| `logger_t.emit_mu` | Logger 实例 | 文件/syslog 后端可变状态、写入/同步/重新打开 | 全局生命周期固定保护（pin）；普通显式 API 可直接获取 | 不允许 `progress_mu/q.wait_mu` | 简单路径可 guard |
| `logger_t.progress_mu` | Logger 实例 | `completed_pos/progress_cv` 完成谓词 | 全局生命周期固定保护（pin）；普通显式 API 可直接获取 | 不允许 `emit_mu/q.wait_mu` | 可 guard |
| `struct logger_queue.wait_mu` | 队列实例 | consumer_waiting 握手 + 条件变量睡眠/唤醒；生产者仅在 waiting 慢速路径获取 | 无 | 不允许 `emit_mu/progress_mu` | 可 guard |
| Console `g_console_mu` | 进程运行域 | Console FILE 输出与 flush | 无 | 格式协调锁 | 带错误检查的 guard |
| `format_mu` | 进程运行域 | 本库 libc 敏感调用与 fork 边界 | Global 控制/生命周期、实例输出、Console 或无 | 资源协调锁 | 显式检查解锁结果 |
| `resource_mu` | 进程运行域 | 映射/fd 取得、登记与消费 | Global 控制/生命周期、实例输出、格式协调或无 | 无 | 租约跨临时 DIR，显式处理 |

<a id="不属于-pthread-lock-的同步机制"></a>

## 不属于 pthread 锁的同步机制

以下机制不能混入锁层级当作普通互斥锁：

- `<active>.logger.lock`：跨进程所有权/独占性；
- `g_ticket`：代次 + 阶段准入；
- 队列槽位 `seq`：MPSC 发布/复用代次；
- 队列 `spill_used[]`：长消息块所有权位图；
- `running`：工作线程停止状态；
- `live_objects`：进程存活对象计数；
- 引用计数（当前生产环境无实例）：独立所有者生命周期。

## 关键禁止关系

<a id="instance-locks-禁止嵌套"></a>

### 实例锁禁止嵌套

当前不允许实例/队列锁互相嵌套，包括：

```text
emit_mu -> progress_mu
progress_mu -> emit_mu
emit_mu -> q.wait_mu
q.wait_mu -> emit_mu
progress_mu -> q.wait_mu
q.wait_mu -> progress_mu
```

如果未来确实需要新增嵌套，必须先修改本表、补 死锁 分析与并发测试。

<a id="global-反向获取禁止"></a>

### 全局反向获取禁止

禁止：

```text
实例锁 -> g_lifetime_lock
实例锁 -> g_control_mu
g_lifetime_lock -> g_control_mu
```

全局读侧 API 必须先取得生命周期固定保护（pin），再进入实例操作。

### Console 独立

`g_console_mu` 不进入 Logger 实例/Global 层级，但允许进入进程格式协调锁。
资源协调锁不允许反向取得格式锁。完整八类锁图和真实正反验证见
[v2 锁契约](v2/LOCKING.md)与[锁跟踪](v2/LOCKDEP.md)。持锁时不要调用会重入
Console/Logger 的宿主回调。

<a id="condition-variable-规则"></a>

## 条件变量规则

`progress_cv` 必须与 `progress_mu` 配对。

`q.wait_cv` 必须与 `q.wait_mu` 配对。

`pthread_cond_wait()` 会原子释放并重新获取对应互斥锁，因此使用词法作用域 guard 时，
guard 代表该作用域对互斥锁的所有权；等待返回后析构函数再负责最终解锁。

## guard 迁移状态

本轮迁移：

- 工作线程 `emit_mu`；
- 工作线程 `progress_mu`；
- 工作线程 `q.wait_mu`；
- 自节奏/强制唤醒 `q.wait_mu`；
- Console `g_console_mu`；
- 显式实例的重新打开/刷新/syslog 指标单锁路径。

刻意保留显式：

- Logger 销毁中 join -> 输出同步 -> 销毁互斥锁的阶段；
- 全局控制/生命周期多锁；

原因不是“guard 不支持”，而是这些路径的锁释放顺序本身就是生命周期/错误契约。
