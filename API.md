# 当前 API 与集成边界

> 默认便利入口现仅借用宿主实例：使用 `logger_default_set(expected, replacement)`。
> 不再提供配置式 Global 初始化、库内销毁或空槽 stderr 回退。
> [默认借用合同](docs/v2/DEFAULT_BORROW.md)与[旧测试映射](docs/v2/DEFAULT_MIGRATION.md)区分当前能力和历史场景。


本文描述 2.0 稳定发布准备中的普通 Logger/Console 接入；当前 ABI 为 SONAME 2 / `LOGGER_2.0`，
当前43个公开 C 符号，清单见 `abi/logger-2.0.symbols`。
[当前 ABI 合同](abi/LOGGER_2_0_CONTRACT.md)按已有可执行快照整理，待最终固定源码验收；本文不表示已经发布。
Audit 不在生产库或安装头文件中；已发布 v1 的62符号合同仅适用于 `release/1.x`。
`logger_fork_reinit()` 与对应构建选项、兼容头文件已移除。

## 进程初始化与当前候选实现（#218）

`int logger_process_init(void)` 以 `0/-errno` 返回并保留调用方 errno。初次初始化、
或普通 libc fork 已取得可靠资源快照的子方重建，成功为0；重复为-EALREADY，
并发为-EAGAIN，注册重入为-EDEADLK。候选初始化失败不发布域，能够完整回滚时
允许显式重试；同步/资源不变量破坏保持可观察的失败状态，不假装可重试。

父方首次使用也必须显式初始化。普通 Logger、Global、Console 与上下文入口不再
按需建域、不等待初始化者：空闲报 ENODEV、初始化中报 EAGAIN、失败后报告保存的
初始化错误，只有显式调用才能重试。进程准入先于配置、拥有者槽与实例字段访问。
已迁移的状态接口使用0/-errno并保留errno；级别设置、TLS上下文与脱敏辅助函数
仍遵循各自当前返回形式。纯配置/脱敏辅助函数不需初始化。
详见[显式进程准入](docs/v2/PROCESS_ADMISSION.md)。

子方普通日志入口不隐式完成重建；必须先调用该接口，再创建新实例。旧实例指针
在子方无效，禁止普通析构；新实例创建失败不清空已建立的进程域。资源隔离、
平台前提和测试入口见 [运行域合同](docs/v2/FORK_RUNTIME.md)。

## 实例生命周期新接口

```c
int logger_create(const logger_config_t *config, logger_t **out);
int logger_destroy(logger_t **owner);
```

两者采用 **0 / -errno，所有正常返回保留调用方 errno**。out 必须指向独占的
空拥有者槽，创建成功才发布实例；非空槽以 -EBUSY 拒绝，失败不覆盖原所有者。
配置只在构造期间借用，长期所需数据建立内部快照。

销毁前必须停止并等待全部借用者退出。入口拒绝不改变槽；准入成功后，库在
取消恢复之前清空 `*owner`，再执行停止、排空、join、同步、关闭和释放。
后续错误不恢复旧指针：普通 I/O 错误仍消费所有权；释放证明失败保守保留内部
分配。NULL 槽不是最终 free 证明，也不能授权使用其他别名重试。
在进程已经就绪的前提下，`owner == NULL` 返回 -EINVAL，有效空槽成功。完整状态表和取消边界见
[创建与消费式销毁合同](docs/v2/LIFECYCLE_API.md)。

旧创建签名、void 销毁和 `logger_destroy_status` 已移除，不保留适配层。
刷新与重开已迁移，实例入口为 `logger_flush(owner)`、`logger_reopen(owner)`；
默认入口为 `logger_default_flush()`、`logger_default_reopen()`。四者均为 **0/-errno**，
所有正常返回保留调用方 errno。已移除旧 status/instance 包装，不能再调用无参
`logger_flush()` 或 `logger_reopen()`。记录、状态/指标查询和Console也已统一；
级别设置、TLS上下文及脱敏辅助函数仍单独迁移。详见[控制操作合同](docs/v2/CONTROL_API.md)。

刷新继续等待进入时捕获的预留完成水位，并同步文件；不代表 Syslog 远端持久化，
不给阻塞 I/O 提供时限。重开可能在切换文件后报错；错误返回不保证无副作用。
指标仍为并发近似快照，不是逐记录接受回执。

## 记录状态

`logger_log`、`logger_log_source`、`logger_global_write`与`logger_log_sync_status`
均返回0/-errno并保留入口errno；`LOGGER_*`、`LOG_*`宏传回函数状态。级别过滤
返回0且不格式化，非法参数和停止状态先于过滤拒绝。GNU `%m` 使用公开入口的errno。

普通同步或队列满后的同步回退返回本次实际输出结果；异步成功表示记录入队，
不表示后端已输出或持久化。容量不足且策略为DROP时返回-EAGAIN并计入丢弃。
负返回可能发生于记录发布、部分写入或完整输出之后，如通知、解锁或取消恢复
失败，因此不能直接重试。严格同步入口继续拒绝正文/整行溢出并强制文件同步，
但不替代等待先前异步水位的flush。完整边界见[记录合同](docs/v2/RECORD_API.md)。

## 诊断与可观测性

十二个状态/指标查询统一返回0/-errno并保留errno，任何失败保持整个输出槽原值。
`logger_get_state(owner, &state)`、`logger_dropped(owner, &count)`和
`logger_global_dropped(&count)`现在通过输出槽返回数值。六个旧void指标入口
失败时的清零规则已明确替换；调用方必须检查rc后使用结果，NULL参数在准入成功
后返回-EINVAL。详细类型与发布时点见[查询合同](docs/v2/QUERY_API.md)。

`logger_get_diagnostics()` 提供无需获取后端锁的统一运行状态快照，包括队列深度/容量、
完成积压、溢出区使用/耗尽、工作线程等待/唤醒、丢弃/回退、输出失败和
粘滞首个错误。它不执行刷新、不执行重连、不进行文件/网络 I/O，也不清理错误状态。

`logger_get_global_diagnostics()` 使用现有全局代次/生命周期固定机制；不会越过
STOPPING/BROKEN 准入门。文件/Syslog 的完整一致输出端指标分别由
`logger_get_file_metrics()` / `logger_get_syslog_metrics()` 提供，因为后端
计数器由 `emit_mu` 保护。文件快照不执行 write/fsync/rotation/reopen，只读取
累计事实；全局门面可用 `logger_get_global_file_metrics()` 通过当前代次固定机制
读取同一份文件后端状态。

问题发现与采样语义见 [OBSERVABILITY.md](docs/OBSERVABILITY.md)。

## 普通日志字符串寿命

调用者的 fmt/source/context 输入必须在对应调用期间有效，不得并发修改。
用户消息仍通过 `vsnprintf` 复制。普通日志入口（仅在已绑定默认实例时提供全局输出）最多保留4095字节正文，
超限时正文尾部明确替换为 ` [truncated]`；未超限正文保持原字节。边界按字节处理，
不承诺保留UTF-8码点。接口使用NUL终止的C字符串文本，内嵌NUL不是二进制消息接口。同步普通输出若因过长元数据耗尽整行缓冲，也在LF前放置同一标记。
需要完整正文确认时使用 `logger_log_sync_status()`，超限返回
`-EOVERFLOW` 且不输出。该行为不新增堆分配或截断计数ABI，原有输出指标仍统计实际发出的记录。
元数据现在按不可变的 `detail/include_*` 需求采集：
只有最终格式真正会使用的模块/上下文/源信息才进入异步队列；需要保留的
模块/源文件基础名/函数继续做有界快照，入队后不再保留业务 DSO 的源信息指针。
容量（不含 NUL）：模块 127、基础文件名 255、函数 127 字节。
超限在末尾以 `~` 标识缩短，按字节而不是 Unicode 代码点处理；这是有意的可见边界变化。
仅作为元数据标签，不能拿被缩短的字符串作业务标识或安全判据。

不新增每条日志的堆分配。私有记录结构增加 512 字节；内部输出行缓冲变为 5120 字节，
容量 >=2 时保留 LF+NUL，0/1 容量也有界处理。正常长度的原有输出字节保持不变。
运行时变更 TZ 的缓存语义未在本轮设计。

## 全局与 Console

宿主创建普通实例，然后 `logger_default_set(NULL, owner)` 将其绑定到便利入口。
使用完毕先 `logger_default_set(owner, NULL)` 等待默认借用退出，再由宿主调用
`logger_destroy(&owner)`。更换默认实例用 `logger_default_set(old, next)`；解绑只
归还借用，不隐含 flush/join。绑定时销毁返回 -EBUSY 并保留拥有者槽。空默认槽的
LOG_* 返回 -ENODEV，不格式化、不输出。业务 SDK 不应隐式改变宿主的默认绑定。
进程初始化仍由宿主显式调用，实际锁/代次/错误规则见[默认借用合同](docs/v2/DEFAULT_BORROW.md)。
Console 是应用终端展示工具，不是业务 SDK 隐式输出通道。其stdio写入使用线程局部SIGPIPE保护，
关闭管道返回 `-EPIPE` 并保留入口errno（如更早已有格式错误则保留首错），不改变宿主全局信号处理函数、屏蔽字或既有待处理信号。

## fork 边界

父方保留实例、队列和工作线程。有效快照的子方在回调中仅关闭库拥有的fd引用、
断开旧域，然后在正常调用位置显式创建新域及实例。内核排除旧运行态映射，
不以 memset/重新赋初值或普通析构修复继承锁。父方已接受记录不由子方重复输出。

仅本库自身的格式化/时间/标准流敏感区间受到协调；POSIX 的多线程fork限制不会
因此消失，宿主其他库的隐藏状态也未被修复。原始系统调用、_Fork/vfork、信号中
fork、注册过程或资源状态不可靠时不能进入重建。当前本地行为和剩余平台验收
必须按 [运行域合同](docs/v2/FORK_RUNTIME.md) 区分，不能宣称任意环境通用安全。


## 跨库句柄和配置

借入 logger_t 时，创建/调用/销毁必须使用同一个实际 Logger 实现；不要在多个 SDK 中各自
静态嵌入不同副本，然后跨副本传递不透明指针。宿主已有另一套日志系统时，使用回调 ABI。

Logger、Console 的当前顶层配置统一采用 Linux UAPI 风格的
`struct_size/version` 合同。当前已知结构必须完整存在；未来更大的结构只有在未知尾部
全部为 0 时才被旧库接受，未知非零语义返回 `E2BIG`。不支持的版本返回
`EPROTONOSUPPORT`，过短结构返回 `EINVAL`。v1 之前的双零配置、旧短前缀和历史旧头文件消费方均不进入当前兼容合同。

结构体本身在 init/create 期间复制为内部快照；其中的字符串指针只在该调用期间借用，
调用者必须保证其有效且不被并发修改。完整规则见 `docs/UAPI.md`。

当前产物使用 SONAME 2 / `LOGGER_2.0`，安装元数据为 `LOGGER_ABI_VERSION=2`；不得冒充 v1 兼容包。

完整集成示例和关闭顺序见 [HOST_OWNED.md](docs/HOST_OWNED.md)。

## 文件所有权契约

即使在 NONE/EXTERNAL 模式下，普通文件目标也只能由单个实例拥有。针对同一已绑定目录/基础文件名
创建独立 Logger 时会返回 EBUSY；应改为共享同一个 Logger。后端会一直保留目录文件描述符以及
`<active>.logger.lock` 的持久 `flock` 租约，直到实例完成处置。最终路径分量如果是符号链接或
硬链接会被拒绝；所在目录必须可信。目录及协调文件必须允许必要的访问；
协调文件不存在时由库以0600创建；已存在时必须可写且通过普通单链接文件检查。

时间戳归档文件绝不会被覆盖；内部轮转要求 Linux 支持 `RENAME_NOREPLACE`，否则返回 ENOTSUP。
重新打开会先打开并验证新文件，再淘汰当前工作文件描述符。切换完成后发生 `close` 错误不会恢复
旧文件描述符。错误仍会通过 I/O 状态、刷新和销毁路径对外可见。

边界、资源成本、错误状态，以及进程退出测试与掉电测试之间的区别，见 `docs/FILE_BACKEND.md`。

## Syslog 非阻塞修订

本地数据报 Syslog 现在使用非阻塞、执行 `exec` 时关闭的套接字；发生背压时会形成明确可见的失败记录，
绝不会隐式转化为重放队列。`logger_config_t` 的 `syslog` 字段沿用当前已知布局；v1 之前的短前缀不再受支持。未来扩展遵守 `docs/UAPI.md` 的尾部追加与未知零尾规则。`logger_get_syslog_metrics()` 提供一致的、面向该输出端的计数器和当前状态。
启动策略默认是 REQUIRED；DEFERRED 会显式接受临时端点故障，并记录粘滞错误状态。重连由单调时钟
冷却周期节流，只由后续日志记录触发，不会由刷新/销毁触发。重连成功也绝不会清除实例历史上的
`first_error`。

字段定义、errno、所有权、ABI 限制、本地线格式，以及“有界系统调用尝试”和“实际墙钟延迟”之间的区别，
详见 [SYSLOG_BACKEND.md](docs/SYSLOG_BACKEND.md)。本次修订并未增加远端持久化投递能力，
子方通过可靠快照显式重建的当前候选边界见上文；绕过处理函数的原始系统调用不支持。

## 默认便利入口的代次与取消

每次真正绑定、替换或解绑产生新代次。数据入口只在一个调用内借用，不返回无保护
实例指针。控制请求也固定进入时的代次，同一指针解绑再绑定不能让旧请求误提交。
控制锁和生命周期锁全部确认释放后才撤销旧实例的借用标记，然后恢复取消策略。
解锁证明失败时保守保留，不以槽指针变化代替最终借用归还。取消恢复失败可能发生在
绑定已提交之后；负返回不普遍表示没有副作用。旧拥有型 Global 记录仅供历史追溯。

## 显式实例/Console 的取消与重入加固

当前所有权契约见 `docs/EXPLICIT_CONSOLE_SCOPE.md`。资源操作会将取消动作延后，直到库内部锁和
临时资源全部释放。构造期间如果存在待处理的延迟取消，会处置尚未返回给调用方的候选实例；如果启用了
异步取消，构造阶段直接以 ENOTSUP 拒绝。已经成功返回的裸指针仍由宿主负责管理。Logger/Console
自身回调中再次嵌套调用 Logger/Console 资源操作（即使目标是另一个实例）会以 EDEADLK 拒绝。
正常的 SDK 到宿主适配器属于顶层调用，继续受到支持。公开结构布局不变，函数类型
按上述ABI 2迁移重新编译；记录与查询拒绝均返回负错误码并保留入口errno。

## 历史 Audit 与旧 ABI

Audit/crypto 不是当前生产 API，安装库不导出 `audit_*`，也不安装 `audit.h`。
旧审计合同、SHA-256历史和SONAME 1由 `release/1.x` 与 `v1.0.0` 定义，不能从当前普通日志库获得。
当前移除边界与未完成事项见 [AUDIT_REMOVAL.md](docs/v2/AUDIT_REMOVAL.md)。

## Console 统一错误与结果

十二个Console接口均返回0/-errno并保留errno，不再使用void配置和-1/errno输出。
`console_init(NULL)`返回-EINVAL；恢复默认值应显式传入默认配置。
`console_stdout_is_tty(&answer)`及stderr对应入口成功写入0/1，ENOTTY表示非终端；
EBADF等为负错误码，失败（包括收尾错误）保留answer原值。原有标准流、标签、
缓冲、过滤、SIGPIPE和取消合同继续保持，完整规则见[Console合同](docs/v2/CONSOLE_API.md)。
