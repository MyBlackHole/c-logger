# 文件和 Syslog 候选描述符的拥有、借用与条件移交（#216、#239）

本阶段实现 #106/#210 的 D1/T1 有限子集：`src/logger_fd.h` 和文件后端
两个真实词法候选路径。它不依赖 PR #215 合并，不表示批准 D1–D6 的全部
范围调整，也不完成其他整数单位类型或完整 AST/跨函数所有权分析。

## 1. 实际对象与操作合同

`struct logger_owned_fd` 和 `struct logger_borrowed_fd` 是不同 C 结构类型，
不是给同一个 int 起两个 typedef 名字。前者保存唯一关闭责任，后者只供
观察，不能直接传入关闭、取出、条件移交或清理函数。

| 操作 | 合同 |
|---|---|
| `logger_fd_own(raw)` | 仅接管已经通过 `logger_fork_open*`、`logger_fork_socket` 等协调入口登记的 fd；不 dup、不分配；失败值为 -1，不能用零初始化表示空。不得从未登记的宿主原始 fd 直接制造本库拥有者 |
| `logger_fd_borrow(&owner)` | 取得借用视图，不延长寿命、不允许在源关闭/移交后继续使用 |
| `logger_fd_number(view)` | 显式 POSIX 整数边界；该整数不因此具有关闭权 |
| `logger_fd_take(&owner)` | 源先失效为 -1，将原始 fd 移交给仍使用 int 的所有者；空源返回 -1 |
| `logger_fd_close_status(&owner)` | 在 `logger_fork_close` 前失效源，由资源协调器注销并关闭；返回 0/-errno，错误后也不能恢复所有权 |
| `logger_fd_cleanup(&owner)` | 普通词法回滚，保留主 errno；不能替代需要回报关闭错误的显式收尾 |
| `logger_fd_fdopendir(&owner)` | 失败保持拥有者不变；成功源失效、同线程持有 DIR 租约，最终由 logger_fork_closedir 释放 |

拥有者指针必须非 NULL、指向已初始化且由当前调用者独占的对象。
负 fd 不被当作有效描述符；fd 0 是有效值。对已移交/关闭的拥有者再次调用
关闭不会触发系统调用。成功移交后的 borrowed 旧副本不再有效。

## 2. 两处生产迁移

`src/logger_file.c:open_candidate`：`logger_fork_openat` 先完整完成资源协调与
位图登记，再以 owned 值承接；检查阶段只取借用整数。校验失败由词法清理
通过 `logger_fork_close` 撤销登记、关闭并保留主错误；成功后显式 take 给
原来的 int 输出参数，其后 install/失败关闭和 fork 资源登记流程不变。

`src/logger_file.c:retention`：独立目录 `logger_fork_openat` 登记后保留
拥有者；`logger_fork_fdopendir` 失败不丢所有权，由词法清理归还登记；
成功后才失效源。DIR 带有同线程 fork 资源租约，整个 readdir 区间保持
原有协调锁，最后必须显式 `logger_fork_closedir`，同步与轮转顺序不变。

当前资源清单保持 19 个直接获取位置不变（历史#106有过22处）；真正的 fdopendir 仍在
`src/logger_fork_fd.c`，私有 `logger_fd.h` 只转调登记包装器，不新增绕开
fork 位图的裸 libc fdopendir/close。没有改写清单、放宽扫描器或加入
生产测试开关。与原 #217 开发分支不同，当前 main 已实施 fork 资源协调，
**不得直接迁入那个分支的裸 openat/fdopendir/close 实现**。
新头文件不安装，公开头、导出符号、SONAME 和 logger_file_t 布局不变。

## 3. 可执行验证

Xmake 注册 `fd-ownership` 分组（需要 build_regression_tests=y）：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_tests=y --build_regression_tests=y
xmake test -g fd-ownership -j1
```

`fd_type_contract` 使用 `fd_ownership_regression` 目标实际选择的 C 编译器。
`tests/scripts/test_fd_types.py` 提供十组负向编译和仅修正类型的正向对照：
borrowed/raw 指针传入 close/take/fdopendir、owned 直接当整数、borrowed/raw
隐式赋给 owned、借用对象 cleanup 均必须拒绝。另有仅编译不运行的逃逸
正例，明确 C 同型复制/显式取裸值仍可编译。缺编译器、缺头文件或未知参数
不算成功拒绝。可以独立使用 `LOGGER_TEST_CC=gcc` 或 `clang` 运行脚本。

`tests/regression/test_fd_ownership.c` 十个独立进程场景以真实 fork 资源
登记入口验证关闭与失效、显式 take、cleanup 保留主错误、close 后真实复用同一整数并合成 EINTR/EIO、
目录移交成功/失败/失败后重试、空拥有者和 fd 0。关闭包装器检查在系统调用
之前源已经失效；真实复用后的描述符必须仍可使用，不能只检查调用次数。
这是假定 Linux 关闭语义的受控回归，不是物理设备掉电测试。

现有 `test_file_acquisition.c`/`run_file_acquisition.sh` 继续通过真实生产
归档验证两处迁移所在路径。文件后端、close EINTR、静态/共享、Sanitizer、
安装消费与最低环境由各自适用工作流验证；局部头文件测试不冒称这些都通过。

## 4. 明确不能证明的内容

C 允许同类型结构赋值、memcpy、显式强转和直接字段访问。因此本阶段不保证
拥有者不可复制，不具备借用检查器/线性类型或完整跨函数逃逸分析。
`logger_fd_number`/`logger_fd_take` 是有意保留的裸 int 边界；禁止从 borrowed
整数再次 own 只能由审查约束，本类型系统无法自动追踪。

logger_file_t 长期字段、Syslog 长期 fd 和其他原始 int 路径没有在本阶段迁移。
字节/记录、索引/计数、代次/序列、标志/枚举、状态和期限/时长仍分别待处理。
D2 的通用 AST/跨函数释放绕过检查没有实现，不能把条件移交 helper 当作其完成。

所有操作要求已有的锁与寿命协议。cleanup 仅负责普通 C 作用域退出，不保证
pthread 取消、longjmp、信号安全、并发回收或异步关闭。Linux close(EINTR)
处理不能推广为所有 POSIX 平台的统一语义；fork 协调器的资源锁证明
失败时故意保持毒化，不得偷偷重试或将未登记的宿主fd当成本库已拥有资源。
生产未新增分配或故障开关；
是否保持完全相同的生成指令/吞吐仍需单独测量，本阶段不承诺性能收益。

## 5. Syslog 临时候选追加迁移（#239、PR #240）

在不修改 `logger_syslog_t.fd` 长期字段、公开 API/ABI 和重连状态机的前提下，
`src/logger_syslog.c:connect_once` 使用 `logger_fd_own(logger_fork_socket(...))`
取得已登记的唯一候选。系统 `connect` 经 `logger_fd_borrow` 和
`logger_fd_number` 得到非拥有整数；失败时 `logger_fd_close_status` 消费候选，
记录关闭失败指标，但恢复原连接错误的 `errno`；成功时使用
`logger_fd_take` 将裸整数显式移交给仍受既有锁保护的长期 `s->fd`。

生产关闭继续由 `logger_fork_close` 消费 fork 位图后完成单次 Linux close。
测试中的 `__wrap_close` 在真实关闭后模拟 EINTR/EIO，并用新的外部 socket
复用旧整数；验证没有第二次关闭、位图拒绝把外部描述符冒充已登记的所有权，
随后又能重新登记并关闭同一整数。额外测试 fd 0 成功移交与双重 close 防护。
新增场景位于 `tests/regression/test_syslog_faults.c`，用 Xmake
`syslog_fault_regression` 注册为 `candidate-fd-zero`、`candidate-eintr-reuse`、
`candidate-eio-reuse`；已有连接错误、延迟启动、重试、真实 Syslog 与 DSO 验证保留。

三个场景还单独注册为 `syslog_candidate_production`，在静态配置下直接
链接真实生产 `logger` 归档；它与 `logger_regression_support` 的白盒验证
具有不同源码构建身份。此目标不冒充共享库内部调用拦截，生产 DSO 仍由现有
真实 syslogd 和共享库测试独立验收。

这个扩展仍是 **Syslog 词法临时拥有者** 的有限类型约束，不提供 C 线性类型、
跨函数 AST 所有权证明、线程/取消生命周期或长期 fd 类型迁移；#210 的 D1–D6
决策和正式发布验收仍独立。是否满足 CI 由 PR 精确提交的原始执行结果确认，
不能把草稿提交视为已验证通过。

## 6. Syslog 长期 fd 退役回执收敛（#241）

长期 `logger_syslog_t.fd` 保持私有裸整数布局；不因内部所有权分析而引入泛化
资源管理框架。原先断开连接时的 `logger_syslog_close` 与 `logger.c` 最终销毁
中的独立裸关闭统一通过私有 `logger_syslog_close_status`：

1. 仍要求实例 `emit_mu`/生命周期已证明独占；先置 `s->fd=-1` 和
   `metrics.connected=0`，再由词法拥有者消费唯一的已登记资源。
2. 经 `logger_fd_close_status` 注销位图并仅关闭一次，返回 0/-errno；失败时
   更新 `close_failures` 和 `last_close_error`，二次调用不再触发系统 close。
3. 原 `logger_syslog_close` 作为忽略错误回执的内部入口，继续供发送故障与构造
   回滚使用；最终销毁则传播关闭错误，同时保留已有主错误优先级和释放次序。
4. `retire-status-eintr/eio`、`retire-destroy-eintr/eio` 分别对私有退役与真正
   `logger_destroy` 验证 close 实际消费后数字即时复用、不重试、指标、最终错误、
   指针消费及 fork 位图拒绝误关宿主描述符，分别在生产静态库与回归支持库验证。

本阶段不是已确认生产损坏的缺陷修复，亦非 C 线性类型或 D2 全程序 AST 证明。
不改变公开 API/ABI、文件轮转及 Syslog 重连协议，也不新增 Audit/断电测试。

## 7. 文件重开临时候选的持有与安装（#243）

`file_reopen_impl` 中的已登记候选经既有 `open_candidate` 取回后，立即交给
`logger_owned_fd` 词法拥有者。旧数据 fd 和目录同步完成前，失败只能通过
`logger_fd_cleanup` 消费新候选的 fork 位图登记，保留原同步错误；同步成功后
`logger_fd_take` 才把候选移交给原有长期字段 `f->fd`。

此修改不改变文件打开/检查、目录同步、旧文件关闭或轮转的时序，不改变
`logger_file_t` 布局。现有 `reopen-sync`、`reopen-dir-sync`、`reopen-close`
保持原验收；新增 `reopen-candidate-eintr-reuse` 和 `reopen-candidate-eio-reuse`
在旧数据 fsync 失败时，先真实关闭新候选，再由宿主复用同号 fd 并合成关闭错误。
验证原 `-EIO` 错误不被关闭错误覆盖、旧 fd 保持可用、重复关闭被协调器拒绝、
下一次重开正常。两种场景分别由回归支持库和生产静态库执行；不把编译器的
C 同型结构复制能力误称为线性类型保证。轮转在归档重命名后的 detached
语义仍须独立审查，未在本阶段迁移。

## 8. 文件轮转新活动候选所有权（#245）

`rotate_impl` 在旧活动文件同步、完成 `rename_noreplace` 及归档目录同步后，
仍保持旧描述符指向归档文件且 `detached=1`。这一点和已有的四个崩溃切点、
目录/新文件 fsync 顺序不变。已通过 fork 位图登记的新活动候选则暂时交给
`logger_owned_fd`；新 inode fsync 时仅借用候选整数，父目录 fsync 成功后才
通过 `logger_fd_take` 安装为新的长期活动 fd。

若新 inode 或活动目录同步失败，显式 `logger_fd_close_status` **先消费新
候选再记录 `switch_error`**，保持原始代码的顺序；作用域清理兜底时源已经
失效，不执行第二次系统关闭。同步主错误继续优先于候选 close(EINTR/EIO)，
旧归档 fd 不被重新当作活动文件追加，且只能通过显式 `logger_reopen` 解除
`detached`。

新增四组故障路径：`rotation-candidate-new-sync-{eintr,eio}-reuse` 和
`rotation-candidate-dir-sync-{eintr,eio}-reuse`。包装器在真实关闭新候选后让
外部 fd 立即复用原数字，再模拟 close 错误；验证主错误、归档未被追加、
关闭位图不误伤宿主 fd、旧 fd 存续及后续显式恢复。复用已有的
`file_backend_regression` 与 `file_reopen_candidate_production` 目标，
不新增生产钩子、专用测试目录或新的构建体系。

上述类型约束只适用于安装前候选，长期 `logger_file_t.{fd,dir_fd,lock_fd}`
仍是现有布局，C 同型复制与完整 D2 跨函数所有权尚未由此证明；
不声明物理掉电认证或正式发布授权。

## 9. 长期文件三 fd 顺序退役与首次错误优先（#247）

`logger_file_close_status` 在最终释放时，继续按活动数据 fd、`lock_fd`、
`dir_fd` 的原次序逐个关闭。每次先将原有长期字段失效为 `-1`，再通过
私有 `logger_owned_fd` 承接已由 fork 位图登记的整数；调用
`logger_fd_close_status` 消费该局部拥有者并仅关闭一次。

如果首个数据 fd 的 Linux close 报告 EINTR/EIO，仍继续消费锁 fd 和目录 fd；
后续即使报告另一个错误，最终回执仍返回最先遇到的错误。失效的三个字段
再次关闭不触发系统调用，`logger_file_close` 的无回执包装保留调用者的
`errno`。不调用 `LOCK_UN`、不删除协调锁文件、不调整各 fd 的实际存活时序。

`close-owned-first-eintr`、`close-owned-first-eio` 分别使首个 fd 在真实
close 后立即被宿主 `/dev/null` 复用并合成不同错误，第二个锁 fd 返回相反的
模拟错误；包装器检查每个字段在自身系统关闭前已失效、第三个目录 fd 仍
完成关闭，外部复用的整数不会被 fork 位图误关。
`close-owned-fd-zero` 验证目录 fd 为 0 时的有效关闭以及重复/NULL 关闭边界。
以上场景复用 `file_backend_regression` 与真实生产静态库
`file_reopen_candidate_production`，不引入新 API、全局锁、动态分配或
性能热路径变化。

本阶段不等于完整 C 线性类型、D2 AST/跨函数生命周期分析、正式
`v2.0.0` 发布或安全审计测试的完成。

## 10. Syslog 长期描述符拥有者类型化（2.0 发布收敛）

Syslog 最后的长期裸 `int fd` 改为**私有** `struct logger_owned_fd`。候选连接成功后
`logger_fd_move` 直接把唯一拥有者移交给私有后端字段，不再经裸整数取出后重新包装；
真正 POSIX `connect/send` 边界始终仅取借用视图。关闭路径通过
`logger_fd_close_status(&s->fd)` 在系统关闭前失效该字段，且仍向最终销毁传播首次
关闭错误。失败连接保留原错误优先级与 `close_failures` 指标；复用整数不可二次关闭。

运行时回归继续复用真实 Syslog 连接、fd=0、EINTR/EIO 关闭后数字复用、断线重连和
最终 `logger_destroy` 的生产及故障测试。`test_fd_types.py` 增加 borrowed/raw 无法隐式
赋入长期拥有者、拥有者无法隐式传入 POSIX 整数 API、borrowed 无法作为 move 源的
编译正反例。

`logger_syslog_t` 与 `logger_t` 仅在不安装的私有头文件中定义，公开头文件与
ABI 2/SONAME/导出符号不变；不添加独立资源管理框架、分配或新锁。C 同型复制、
非法显式 `own(raw)`、直接字段访问、跨函数别名与宿主并发销毁仍无法由此静态阻止。
**本改动仅收敛 D1/T1 中 Syslog 长期 fd 一项**，不构成 D1–D6、TSan fork 跳过、
宿主风险或发布签收已完成。
