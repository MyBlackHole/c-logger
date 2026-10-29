# 资源所有权矩阵

> 本文固定源码中的私有 typedef 名称保留为原记录；当前具名类型对照见 [编码规范](v2/CODING_STYLE.md)。类型拼写变化不扩大下面原始验证范围。


> 当前创建/销毁以[生命周期合同](v2/LIFECYCLE_API.md)为准：直接使用拥有者槽与负错误码，不保留旧接口。
> [默认槽仅借用宿主实例](v2/DEFAULT_BORROW.md)；实例、队列与工作区使用[运行域私有映射](v2/FORK_RUNTIME.md)。

> 现行设计入口见 [v2/README.md](v2/README.md)。本文保留的 Audit 专属名称、结构与行为仅为历史背景；
> Audit/密码学模块已退役，不属于当前 API、对象图或验证范围。普通 Logger 条款仍适用。

该矩阵记录 c-logger 中重要的进程域、私有映射、文件描述符、线程和同步资源。
它补充[资源所有权规范](RESOURCE_OWNERSHIP.md)：结合当前实现逐项记录取得、持有、转移和释放。
本次校准以 main `32d4e8748aa6fe276e1a74c848609546fdfce45a` 的生产实现为依据；
矩阵描述不等于新的动态验证或全程序资源审计，测试身份和范围仍以原始回执为准。

状态含义：

- **AUTO**：词法作用域所有者使用 Linux 风格自动清理完成回滚。
- **EXPLICIT**：最终处置保持显式，因为顺序或释放错误具有业务语义。
- **SHARED**：多个使用者通过明确协议共享访问，仍须有最终所有者和释放前的静默期证明。
- **BORROWED**：当前代码可以使用资源，但绝不能释放。
- **REFCOUNTED**：多个独立生命周期所有者持有计数引用；当前没有生产资源使用该模式。

AUTO/EXPLICIT 描述清理方式，SHARED/BORROWED/REFCOUNTED 描述访问或生命周期关系，
两者可以组合；AUTO 不表示底层回收必然成功。

## Logger 实例

| 资源 | 获取/创建 | 当前/持久所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| `logger_t` 私有映射 | `create_logger` 中的 `logger_runtime_calloc` | 构造候选，随后为宿主的拥有者槽 | `logger_create` 成功写入空 `*out` | `logger_destroy(&owner)` / 内部候选回滚，最终 `logger_runtime_free` | EXPLICIT |
| 进程活动对象令牌 | `logger_process_object_acquire` | 进程级对象计数/门控 | 与 Logger 构造/销毁配对 | `logger_process_object_release` | SHARED（不是 REFCOUNTED） |
| `emit_mu` | `pthread_mutex_init` | `logger_t` | 无 | 外部借用者静默、工作线程退出并完成最后同步后销毁，随后关闭后端 | EXPLICIT |
| `progress_mu/progress_cv` | pthread 初始化 | `logger_t` | 无 | 外部借用者静默、工作线程退出并拆除队列后销毁 | EXPLICIT |
| 紧凑队列存储 `q.slots` | `logger_queue_init` 中的 `logger_runtime_calloc` | `__free(runtime_free)` 词法所有者，随后为 `logger_queue_t` | `q->slots = no_free_ptr(slots)` | 静默且队列同步对象销毁成功后，`logger_queue_destroy` 调用 `logger_runtime_free` | AUTO -> SHARED/EXPLICIT |
| 长消息溢出池 `q.spills` | `logger_queue_init` 中的 `logger_runtime_calloc` | `__free(runtime_free)` 词法所有者，随后为 `logger_queue_t` | `q->spills = no_free_ptr(spills)` | 与槽位相同的静默前提；队列析构先释放 spills，再释放 slots | AUTO -> SHARED/EXPLICIT |
| `q.wait_mu/q.wait_cv` | `logger_queue_init` 中的 pthread 初始化 | `logger_queue_t` | 初始化成功后才发布 slots/spills | 生产者静默、工作线程 join 后由 `logger_queue_destroy` 销毁 | EXPLICIT |
| 工作线程工作区及其六个缓冲区 | `logger_runtime_calloc/logger_runtime_malloc` | `__free(runtime_free)` 词法所有者，随后为工作区/`logger_t` | `no_free_ptr/return_ptr`，随后赋给 `l->worker_workspace` | 工作线程退出后由 `logger_worker_workspace_destroy` 调用 `logger_runtime_free` | AUTO -> EXPLICIT |
| 工作线程 | `pthread_create` | `logger_t` | 无 | 协作式停止 + `pthread_join` | SHARED/EXPLICIT |
| 普通显式 API 的 `logger_t *` 参数 | 宿主提供存活实例 | 宿主始终保持所有权 | 无 | 普通调用不释放；宿主停止并等待借用者后显式销毁 | BORROWED |

构造失败不对外发布半成品，按 `logger_ctor_state_t` 回滚已取得的资源。
已启动的工作线程须先停止并 join；同步或回收证明失败则保守保留，不能称为全部资源已归还。
只要函数仍使用基于 goto 的资源回退，就不会只迁移其中一部分为 `__free`。

公开销毁入口拒绝时保留拥有者槽，仍有默认借用时返回 `-EBUSY`；进入拆除前先清槽。
后续 I/O 错误不恢复旧指针；锁、join、同步对象或映射释放证明失败时，公开槽为空也不证明物理释放。
旧别名不得重试销毁；具体边界见[资源所有权规范](RESOURCE_OWNERSHIP.md)。

## 全局 Logger

| 资源 | 获取/创建 | 所有者/使用者 | 转移/发布 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| 默认槽 `g_logger`（`runtime->global_logger`） | 宿主先创建实例，再调用 `logger_default_set` 绑定 | 实例由宿主持有；默认槽只借用 | 控制锁 + 生命周期写锁下更新槽并发布新票据，不转移实例所有权 | 解绑/替换归还默认借用；最终由宿主 `logger_destroy(&owner)` | BORROWED/SHARED |
| 默认入口读侧使用 | 取得并复核默认绑定票据 | 生命周期读锁保护当前调用中的借用 | 无所有权转移 | 退出读侧并确认解锁，不销毁实例 | BORROWED |
| `g_control_mu`（`runtime->global_control_mu`） | `logger_runtime_create` 中 `pthread_mutex_init` | 进程运行域 | 运行域就绪后供绑定控制使用 | 域回滚或健康空域卸载时 `logger_runtime_discard` | SHARED/EXPLICIT |
| `g_lifetime_lock`（`runtime->global_lifetime_lock`） | `logger_runtime_create` 中 `pthread_rwlock_init` | 进程运行域 | 运行域就绪后供默认准入使用 | 同上；解绑或单实例销毁不拆除该锁 | SHARED/EXPLICIT |
| 代次/阶段票据 `runtime->global_ticket` | 新域中 `atomic_init` | 默认绑定状态机 | 原子代次/阶段转换 | 随运行域存储回收，不是实例引用计数 | SHARED |

原子凭据用于准入与版本门控。它不能替代生命周期读写锁，也不能单独保证 `g_logger` 存活。
生命周期读锁是一种固定机制：借用期间阻止最终释放，但它不是计数引用。
解绑只等待默认入口在途调用，不刷新、排空或销毁实例。只有相关锁成功解锁后才清除
旧实例的 `default_borrowed` 管理性标记；证明失败保留借用并使绑定层失败关闭。
标记清零不证明其他显式借用者已经退出。

## 文件后端

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| 绑定目录 fd `dir_fd` | `bind_path` 中 `logger_fork_open`，再由 `logger_fd_own` 接管 | `logger_file_t.dir_fd` | 已登记 fd 写入空拥有者字段 | `logger_file_close_status` 经 `logger_fd_close_status` 关闭 | EXPLICIT |
| 协调 fd `lock_fd` | `logger_file_reserve` 中 `logger_fork_openat`，再由 `logger_fd_own` 接管 | `logger_file_t.lock_fd` | 已登记 fd 写入空拥有者字段 | 同上；关闭此引用，不主动 LOCK_UN 或删除锁文件 | EXPLICIT |
| 活动 fd `fd` | `open_candidate` 中 `logger_fork_openat` | 带 `logger_fd_cleanup` 的词法候选，随后为调用方/后端 | `*out = logger_fd_move(&fd)`；`install_fd` 再移动到 `f->fd` | 失败候选回滚；已安装 fd 在后端拆除/轮转中显式关闭或替换 | AUTO -> EXPLICIT |
| 保留策略扫描 fd | `logger_fork_openat(dirfd, ".", O_DIRECTORY 等标志, 0)` | 带 `logger_fd_cleanup` 的词法拥有者 | `logger_fd_fdopendir` 成功才使源失效并移交 DIR；失败保留源 | 移交前词法回滚；移交后由 `logger_fork_closedir` 消费 | AUTO -> EXPLICIT |
| 保留策略 `DIR *` 及资源租约 | `logger_fd_fdopendir` / `logger_fork_fdopendir` | 保留策略函数；同线程持有资源协调租约 | 遍历期间保持 fd 登记与资源锁 | 同线程显式 `logger_fork_closedir`，不能直接 libc `closedir` | EXPLICIT |
| 轮转/重开候选与退役 fd | `open_candidate`，旧 `f->fd` 保持独立拥有 | 词法候选；安装时将旧拥有者移动至 retiring | 验证/所需同步成功后 `install_fd`；源立即失效 | 新候选失败时回滚，旧 fd 退役时 `logger_fd_close_status` | AUTO/EXPLICIT |

文件关闭、fsync、目录同步以及事务式替换错误都属于行为契约的一部分，
不能隐藏在“尽力清理”的析构器后面。
长期 fd 使用 `struct logger_owned_fd`，借用整数不携带释放权。关闭先使拥有者失效；
只有资源门禁/锁获取成功且登记有效，才注销登记并执行单次 close。门禁或锁获取失败时，
底层 fd 与登记仍可能保留在域内，字段已空不证明关闭完成，也不能凭旧整数重试。
实际执行 Linux close 即使报 EINTR，也不恢复源或重试。
重开/轮转在新 fd 已装入后仍可能因旧 fd 关闭失败返回错误，不能把负返回当成未发生切换。
轮转中已归档的旧 fd 不再追加；新候选失败则保留 detached/切换错误，等待显式重开恢复。

## Syslog 后端

| 资源 | 获取/创建 | 所有者 | 转移点 | 最终释放 | 模式 |
|---|---|---|---|---|---|
| 候选套接字 fd | `connect_once` 中 `logger_fork_socket`，再由 `logger_fd_own` 接管 | 本地连接事务 | 连接成功才执行 `s->fd = logger_fd_move(&candidate)` | 连接失败显式 `logger_fd_close_status(&candidate)`，保留连接主错并记录关闭指标 | EXPLICIT |
| 已安装套接字 fd | 成功的 `connect_once` | `logger_syslog_t.fd` | 移动后 candidate 失效 | `logger_syslog_close_status` / 重连，经 `logger_fd_close_status` 单次关闭 | EXPLICIT |

候选套接字清理有意保持显式，因为关闭失败需要更新 Syslog 指标。
若替换为通用 `__free(close_fd)`，会丢失可观察的记账信息。
带类型拥有者的先失效及资源门禁失败时的保留规则与文件 fd 相同。

## Audit 子系统

> 以下表格和段落原样保留为已退役 Audit 的历史所有权记录，不参与当前资源矩阵的实现符合性判断。
> 本节的“当前”“不再”和“保持”均对应原历史阶段，不能据此恢复 Audit 或推导当前能力。

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
| 进程运行域映射 | `logger_runtime_create` 中 `mmap` + `MADV_DONTFORK` | 初始化候选，成功后由进程域根指针持有 | `logger_process_init` 中 `logger_runtime_publish` 后发布 READY | 初始化失败回滚；卸载仅归还健康空域 | EXPLICIT/SHARED |
| `resource_mu/format_mu/console_mu` | `logger_runtime_create` 中逐个 `pthread_mutex_init` | 进程运行域 | 随域发布；不放在可继承的静态 mutex 中 | `logger_runtime_discard` 按已初始化数量逆序销毁 | SHARED/EXPLICIT |
| 私有分配映射及登记链 | `logger_runtime_calloc/malloc` 中 `mmap` + `MADV_DONTFORK` | 调用方结构/词法所有者；域保留分配登记 | 完成映射配置与登记后才返回负载指针 | `logger_runtime_free` 成功 munmap 才移除登记；失败保留并毒化域 | AUTO/EXPLICIT |
| fd 登记位图 `owned_fds` | `grow_for` 中 `mmap`，高 fd 时 `mremap` | 进程运行域；受 `resource_mu` 保护 | 新 fd 登记成功才交给后端；映射/扩容失败回滚新 fd | 最后登记 fd 关闭后 `release_empty` 归还映射；失败保留并毒化域 | SHARED/EXPLICIT |
| fork 子方 fd 数值快照/继承位图 | prepare 在格式锁 -> 资源锁下取得现有位图地址/长度 | 父方继续原域；有效子方只使用 TLS 数值快照 | child handler 按位图单次关闭继承 fd，清位且不访问已排除的旧域 | 子方显式初始化中 `logger_fork_fd_child_reinit` 归还已消费位图；失败不重复 close | EXPLICIT |
| atfork 注册 | 显式 `logger_process_init` 中 `pthread_atfork` | 当前实际库实现的进程注册状态 | 原子初始化票据串行化，成功后记录 handlers_registered | 不提供普通 API 注销；单实例销毁不撤销注册 | SHARED |
| PID/初始化票据 | 静态原子变量；显式初始化时更新 | 进程准入协议；子方拒绝路径也需访问 | READY 发布/子方失效与显式重建 | 不属于单个实例，也不是对象引用计数 | SHARED |
| 活动对象计数 `runtime->live_objects` | 新域 `atomic_init`，构造/销毁配对 acquire/release | 进程生命周期协议 | 原子计数递增/递减，不转移对象所有权 | 配对 release；计数归零不证明其他保留映射已经释放 | SHARED |

旧版受控 fork 辅助函数与 `/proc/self/task` 扫描已移除；进程防护不接管宿主线程或进程创建。
运行域和实例私有映射均不继承；fd 位图有意保留供有效 fork 子方消费，不能将它也标成 DONTFORK。
原始系统调用、无可信快照等路径不支持重建；父方实例指针在子方失效，不进行普通销毁。

域初始化失败不发布半成品；同步对象或域映射回滚失败时由 `retained_bootstrap` 保留候选，
后续初始化拒绝复用。普通分配的 munmap 失败保留登记；若 munmap 已成功、仅随后的协调锁
解锁失败，映射已消费，解锁错误通过域诊断保留，不能重新登记失效地址或重试释放。
卸载前宿主须停止调用者并销毁实例；析构入口只处理无存活对象、无分配/描述符登记且无资源错误的空域，
不替代宿主静默期，也不强行回收错误保留域。

## 工作线程/队列的共享生命周期

队列槽位、溢出池和工作线程工作区都只有一个结构性所有者，
但会被生产者/消费者并发使用。下图只表示 `dispose_body` 的成功拆除路径；
同步、唤醒、join、同步对象销毁或映射回收证明失败时不得强行完成最终释放：

```text
宿主停止并等待外部借用者；如有默认绑定，先成功归还默认借用
    ->
logger_destroy 清空拥有者槽，发布 STOPPING，再关闭 running
    ->
唤醒工作线程
    ->
工作线程排空并退出
    ->
pthread_join 成功
    ->
复核同步证明；保存 lifecycle_error，无该错误时执行最后输出同步，再解锁
    ->
销毁工作区
    ->
成功销毁队列条件变量/互斥锁
    ->
logger_runtime_free 溢出池，再释放紧凑槽位
    ->
销毁进度/输出同步对象
    ->
关闭文件/Syslog 后端（普通关闭错误保留首错，不阻止已证明安全的后续回收）
    ->
发布 STOPPED，归还进程活动对象计数
    ->
logger_runtime_free 实例映射；只有成功才给出真实最终释放回执
```

词法作用域清理不能替代这个“释放前等待工作线程退出”的要求。
返回值与释放回执分别判定：lifecycle/I/O 错误可能与实际完成回收同时发生，
公开槽为空也可能伴随内部保守保留。构造回滚按已完成阶段选择对应释放路径，不能直接套用完整成功路径。

## 引用计数审计

当前没有任何生产资源需要 REFCOUNTED 生命周期：

- 显式 Logger 实例只有一个宿主所有者以及有界借用者；
- 默认槽只借用宿主实例，默认调用由代次/准入和读写锁保护；
- 工作线程通过协作式停止和 join 结束生命周期；
- 队列/工作区存储只有一个结构性所有者，并遵循释放前 join；
- `live_objects` 只统计进程对象，用于 fork/生命周期门控，不控制任何单个对象的最终释放。

如果未来资源存在可以逃逸这些生命周期边界的独立所有者，
应在引入其 get/put 协议的同一次修改中同步更新该矩阵和 `REFCOUNTING.md`。

## 本次审计后的迁移决定

当前生产路径使用的作用域清理：

- 队列/工作区私有映射的 `__free(runtime_free)` 候选回滚与显式移交；
- 文件后端 `open_candidate` 的带类型候选 fd；
- `logger_fd_fdopendir` 前的文件保留策略目录 fd。

有意继续保持显式：

- Logger 构造函数的 goto 回退；
- 任何其错误需要返回的关闭/fsync/closedir；
- 带有指标副作用的 Syslog 套接字候选清理；
- 默认绑定多锁生命周期协议与借用归还；
- 工作线程和队列共享生命周期；
- 运行域初始化、资源登记及错误保留。

历史 Audit 阶段曾迁移写入器锁候选 fd 和恢复文件候选 fd；恢复段聚合及 Audit 多锁协议
当时保持显式。这里只记录原阶段决定，不表示当前仍存在这些生产资源。

未来如果继续转换所有权图，应在同一次修改中更新该矩阵。
