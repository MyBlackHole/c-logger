# 文件候选描述符的拥有、借用与条件移交（#216）

本阶段实现 #106/#210 的 D1/T1 有限子集：`src/logger_fd.h` 和文件后端
两个真实词法候选路径。它不依赖 PR #215 合并，不表示批准 D1–D6 的全部
范围调整，也不完成其他整数单位类型或完整 AST/跨函数所有权分析。

## 1. 实际对象与操作合同

`struct logger_owned_fd` 和 `struct logger_borrowed_fd` 是不同 C 结构类型，
不是给同一个 int 起两个 typedef 名字。前者保存唯一关闭责任，后者只供
观察，不能直接传入关闭、取出、条件移交或清理函数。

| 操作 | 合同 |
|---|---|
| `logger_fd_own(raw)` | 仅接管已经通过 `logger_fork_open*` 完成登记的 fd；不 dup、不分配；失败值为 -1，不能用零初始化表示空。不得从未登记的宿主原始 fd 直接制造本库拥有者 |
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

logger_file_t 长期字段、Syslog fd 和其他原始 int 路径没有在本阶段迁移。
字节/记录、索引/计数、代次/序列、标志/枚举、状态和期限/时长仍分别待处理。
D2 的通用 AST/跨函数释放绕过检查没有实现，不能把条件移交 helper 当作其完成。

所有操作要求已有的锁与寿命协议。cleanup 仅负责普通 C 作用域退出，不保证
pthread 取消、longjmp、信号安全、并发回收或异步关闭。Linux close(EINTR)
处理不能推广为所有 POSIX 平台的统一语义；fork 协调器的资源锁证明
失败时故意保持毒化，不得偷偷重试或将未登记的宿主fd当成本库已拥有资源。
生产未新增分配或故障开关；
是否保持完全相同的生成指令/吞吐仍需单独测量，本阶段不承诺性能收益。
