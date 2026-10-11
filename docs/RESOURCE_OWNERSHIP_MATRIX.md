# 资源所有权矩阵

> 现行设计入口见 [v2/README.md](v2/README.md)。本文保留的 Audit 专属名称、结构与行为仅为历史背景；
> Audit/密码学模块已退役，不属于当前 API、对象图或验证范围。普通 Logger 条款仍适用。

该矩阵用于持续审计 c-logger 中重要的进程级资源、堆资源、文件描述符、线程和同步资源。
它补充 `RESOURCE_OWNERSHIP.md`：其中要求回答的四个问题，会在这里结合当前实现逐项记录。

状态含义：

- **AUTO**：词法作用域所有者使用 Linux 风格自动清理完成回滚。
- **EXPLICIT**：最终处置保持显式，因为顺序或释放错误具有业务语义。
- **SHARED**：生命周期由并发协议管理，而不是由词法作用域管理。
- **BORROWED**：当前代码可以使用资源，但绝不能释放。
- **REFCOUNTED**：多个独立生命周期所有者持有计数引用；当前没有生产资源使用该模式。

## Logger 实例

| 资源 | 获取/创建 | 当前/持久所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| `logger_t` 堆对象 | `create_logger` 中的 `calloc` | 构造过程，随后为宿主/调用方 | 构造函数成功返回 | `logger_destroy_status/logger_dispose_internal` | EXPLICIT |
| 进程活动对象令牌 | `logger_process_object_acquire` | 进程级对象计数/门控 | 与 Logger 构造/销毁配对 | `logger_process_object_release` | SHARED（不是 REFCOUNTED） |
| `emit_mu` | `pthread_mutex_init` | `logger_t` | 无 | 后端/工作线程拆除后销毁 | EXPLICIT |
| `progress_mu/progress_cv` | pthread 初始化 | `logger_t` | 无 | 工作线程 join 后销毁 | EXPLICIT |
| 紧凑队列存储 `q.slots` | `logger_queue_init` 中的 `calloc` | 词法所有者，随后为 `logger_queue_t` | `q->slots = no_free_ptr(slots)` | `logger_queue_destroy` | AUTO -> EXPLICIT |
| 长消息溢出池 `q.spills` | `logger_queue_init` 中的 `calloc` | 词法所有者，随后为 `logger_queue_t` | `q->spills = no_free_ptr(spills)` | 生产者/工作线程静默后由 `logger_queue_destroy` 释放 | AUTO -> SHARED/EXPLICIT |
| 工作线程工作区 | 工作区构造过程中的分配 | 工作区对象，随后为 `logger_t` | `no_free_ptr/return_ptr` | 工作线程 join 后由 `logger_worker_workspace_destroy` 释放 | AUTO -> EXPLICIT |
| 工作线程 | `pthread_create` | `logger_t` | 无 | 协作式停止 + `pthread_join` | SHARED/EXPLICIT |
| 显式 API 的 `logger_t *` 参数 | 宿主 | 宿主始终保持所有权 | 无 | 仅由宿主释放 | BORROWED |

构造失败清理继续采用“全有或全无”的显式回退图。
只要函数仍使用基于 goto 的资源回退，就不会只迁移其中一部分为 `__free`。

## 全局 Logger

| 资源 | 获取/创建 | 所有者/使用者 | 转移/发布 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| `g_logger` | `logger_create` 候选实例 | 全局生命周期控制器 | 在控制 + 生命周期协议下发布 | 关闭控制器 | SHARED |
| 全局读侧使用 | 已存在的 `g_logger` | 持有生命周期读侧固定时借用 | 无 | 仅解除固定，绝不销毁 | BORROWED |
| `g_control_mu` | 静态初始化 | 进程全局控制器 | 无 | 进程生命周期 | SHARED |
| `g_lifetime_lock` | 静态初始化 | 全局准入协议 | 无 | 进程生命周期 | SHARED |
| 代次/阶段凭据 | 静态原子变量 | 全局状态机 | 原子代次转换 | 进程生命周期 | SHARED |

原子凭据用于准入与版本门控。它不能替代生命周期读写锁，也不能单独保证 `g_logger` 存活。
生命周期读锁是一种固定机制：借用期间阻止最终释放，但它不是计数引用。

## 文件后端

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| 绑定目录 fd `dir_fd` | 文件初始化/预留期间 `open` | `logger_file_t` | 后端构造/移动成功 | `logger_file_close_status` | EXPLICIT |
| 协调 fd `lock_fd` | `openat` | `logger_file_t` | 后端构造/移动成功 | `logger_file_close_status` | EXPLICIT |
| 活动 fd `fd` | `open_candidate` | 词法候选，随后为调用方/后端 | `take_fd(fd)`，随后安装 | 后端拆除/轮转中关闭或替换 | AUTO -> EXPLICIT |
| 保留策略扫描 fd | `openat(".", O_DIRECTORY)` | 词法变量 | 成功的 `fdopendir` 会消费它 | `closedir` | AUTO -> EXPLICIT |
| 保留策略 `DIR *` | `fdopendir` | 保留策略函数 | 无 | 显式 `closedir`，因为错误有意义 | EXPLICIT |
| 轮转替代 fd | `open_candidate` | 轮转/重新打开事务 | 验证/同步成功后才安装 | 根据事务结果显式关闭旧/新 fd | EXPLICIT |

文件关闭、fsync、目录同步以及事务式替换错误都属于行为契约的一部分，
不能隐藏在“尽力清理”的析构器后面。

## Syslog 后端

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| 候选套接字 fd | `connect_once` 中的 `socket` | 本地连接事务 | 连接成功后才执行 `s->fd = fd` | 后端关闭/重连 | EXPLICIT |
| 已安装套接字 fd | 成功的 `connect_once` | `logger_syslog_t` | 连接后赋值 | `logger_syslog_close` / 重连 | EXPLICIT |

候选套接字清理有意保持显式，因为关闭失败需要更新 Syslog 指标。
若替换为通用 `__free(close_fd)`，会丢失可观察的记账信息。

## Audit 子系统

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| `audit_runtime_t` 堆对象 | Audit 初始化期间 `calloc` | 初始化事务，随后为 Audit 全局运行时 | `g_runtime = candidate` 发布 | `dispose_runtime` | EXPLICIT/SHARED |
| 活动日志预留资源 | `logger_file_reserve` | 初始为 `audit_runtime_t.reserved_log` | `logger_create_reserved_file` 成功后移动到私有 Logger；源立即恢复 `LOGGER_FILE_EMPTY` | 私有 Logger 的文件后端销毁 | EXPLICIT/MOVED |
| 检查点预留资源 | `logger_file_reserve` | `audit_runtime_t.checkpoint_owner` | 不转移 | `dispose_runtime -> logger_file_close_status` | EXPLICIT |
| 活动文件/检查点 `dir_fd/lock_fd` | 预留内部 `open/openat` | 各自所属 `logger_file_t` | 随整个 `logger_file_t` 预留移动 | 对应 `logger_file_close_status` | EXPLICIT/MOVED |
| 恢复普通文件 fd | `open_regular_at` 中 `openat` | 词法 fd | 成功 `fdopen` 后由 `take_fd` 失效源所有者 | 调用方显式 `fclose` | AUTO -> EXPLICIT |
| 恢复 `FILE *` | `fdopen` | 恢复操作 | 无 | 显式 `fclose`；错误参与恢复结果 | EXPLICIT |
| 归档扫描 fd | `openat(dirfd, ".")` | 词法/扫描事务 | 成功 `fdopendir` 后由 DIR 接管 | `closedir` | EXPLICIT |
| 恢复段数组 / 段名 | `calloc/strdup` | 恢复操作 | 存入段聚合 | 清理循环 / `free` | EXPLICIT |
| Audit 控制/操作互斥锁 | 静态初始化 | Audit 生命周期 | 无 | 进程生命周期 | SHARED |

Audit 不再维护独立写入器锁或独立长期日志目录 fd；跨进程写入器独占性
由活动文件/检查点对应的文件后端预留资源承担。恢复/检查点直接使用
这些所有者持有的 `dir_fd + basename`，不把 fd 转回路径名。

Audit 的持久化最终处置保持显式，因为检查点、文件关闭、同步顺序和错误都属于
恢复/提交语义。

## 进程/fork 防护

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| atfork 注册 | `pthread_once/pthread_atfork` | 进程 | 无 | 进程生命周期 | SHARED |
| 活动对象计数 | 原子 acquire/release | 进程生命周期协议 | 计数递增/递减 | 配对 release | SHARED |

旧版受控 fork 辅助函数与 `/proc/self/task` 扫描已移除；进程防护不接管宿主线程或进程创建。

## 工作线程/队列的共享生命周期

队列槽位、溢出池和工作线程工作区都只有一个结构性所有者，
但会被生产者/消费者并发使用，因此销毁顺序固定：

```text
停止发布/running
    ->
唤醒工作线程
    ->
工作线程排空并退出
    ->
pthread_join 成功
    ->
记录/返回工作线程 lifecycle_error（不再阻止安全回收）
    ->
销毁工作区
    ->
成功销毁队列条件变量/互斥锁
    ->
释放紧凑槽位 + 溢出池
    ->
销毁进度/输出同步对象
    ->
最终释放内存
```

词法作用域清理不能替代这个“释放前等待工作线程退出”的要求。

## 引用计数审计

当前没有任何生产资源需要 REFCOUNTED 生命周期：

- 显式 Logger 实例只有一个宿主所有者以及有界借用者；
- 全局 Logger 读者持有读写锁生命周期固定；
- 工作线程通过协作式停止和 join 结束生命周期；
- 队列/工作区存储只有一个结构性所有者，并遵循释放前 join；
- Audit 运行时由自身生命周期/操作协议管理；
- `live_objects` 只统计进程对象，用于 fork/生命周期门控，不控制任何单个对象的最终释放。

如果未来资源存在可以逃逸这些生命周期边界的独立所有者，
应在引入其 get/put 协议的同一次修改中同步更新该矩阵和 `REFCOUNTING.md`。

## 本次审计后的迁移决定

当前已迁移：

- Audit 写入器锁候选 fd；
- `fdopen` 前的 Audit 恢复普通文件候选 fd；
- 文件后端 `open_candidate` 返回的候选 fd；
- `fdopendir` 前的文件保留策略目录 fd。

有意继续保持显式：

- Logger 构造函数的 goto 回退；
- 任何其错误需要返回的关闭/fsync/closedir；
- 带有指标副作用的 Syslog 套接字候选清理；
- 全局/Audit 多锁生命周期协议；
- 工作线程和队列共享生命周期；
- 恢复段聚合清理。

未来如果继续转换所有权图，应在同一次修改中更新该矩阵。
