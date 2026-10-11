# c-logger 2.0.0 生产稳定版发布说明

本文描述 2.0.0 的范围与合同，正式发布状态以获批标签及对应 GitHub Release 为准。
最终源码由获批标签解析；最终验收证据独立记录 `SOURCE_COMMIT` 完整 SHA、标签对象 ID、
工作流 run ID 和产物 SHA-256，不要求在验收后修改本文或填入本文自身的提交号。
本文所列准备基线证据不等于最终提交已经通过验收，也不表示生产部署已经完成。

## 产品与支持范围

普通 Logger/Console 提供文件、stderr、本地 Syslog，同步/异步日志、文件轮转、
显式实例与全局便利入口，以及错误状态、丢弃/回退、队列和后端诊断。

初始支持范围为 Linux x86_64/ELF64、Linux >=5.10、glibc >=2.31、GNU C11，
GCC >=9、Clang >=10；公开头文件支持 C++11 消费方。项目构建、测试、安装与打包
以 Xmake 为准，CI 固定 3.1.1；CMake 与 pkg-config 是下游消费接口。
musl、AArch64、32 位和 NFS/SMB 不在本次范围。详见[支持矩阵](v2/SUPPORT_MATRIX.md)。

## ABI 破坏与迁移

- 使用 SONAME `liblogger.so.2`、ELF 符号版本 `LOGGER_2.0` 和 47 个公开 C 符号。
  不可替换 SONAME 1 库；必须使用同版头文件重新构建并验收消费方，不承诺 v1 源码兼容。
- 删除 Audit/crypto 产品、`audit.h` 和 `audit_*` 导出。普通日志不提供审计事务、
  哈希链、不可变存储或可信锚点。旧审计用户继续使用 `release/1.x`，或迁移独立组件。
- 删除 `logger_fork_reinit()`、旧兼容头和构建开关。继承运行时的原始 fork 子进程
  由 PID/atfork 防护拒绝，应 exec 后初始化；不扩大 fork 后可调用范围。
- 配置使用当前默认初始化器及 `struct_size/version` 合同；未知版本、过短结构、
  非零未知尾部明确拒绝。实例的创建、使用、销毁须使用同一实际 Logger 实现。
- 文件目标实行单所有者：同一目录/基础名的独立实例返回 EBUSY，应共享实例。
  内部轮转要求 `RENAME_NOREPLACE`，不以覆盖归档降级；目录必须可信。

具体公开合同见 [API](../API.md)、[配置规则](UAPI.md)和[宿主持有集成](HOST_OWNED.md)。
上述版本身份和符号数量已经存在于准备基线；[当前 ABI 合同](../abi/LOGGER_2_0_CONTRACT.md)
按现有可执行快照整理，最终稳定 ABI/API 的发布确认须与最终源码验收一并记录，
不能仅凭旧开发文案断言实现未完成，也不能仅凭本稿断言合同已经冻结。

## 已合并的修正与工程保障

- [#165](https://github.com/MyBlackHole/c-logger/pull/165)：轮转测试使用独立目录，
  校验完整记录顺序、唯一性及关闭结果，并增加拒绝假阳性的变异测试。
- [#167](https://github.com/MyBlackHole/c-logger/pull/167)：明确四处全局生命周期 CAS 的
  保守 C11 内存序组合，增加评审清单和负向变异门禁；不把此前疑义写成已复现的运行时故障。
- [#168](https://github.com/MyBlackHole/c-logger/pull/168)：对齐当前并发/资源协议，
  显式安装当前设计文档并验证重定位后的相对链接。
- [#187](https://github.com/MyBlackHole/c-logger/pull/187)、
  [#182](https://github.com/MyBlackHole/c-logger/pull/182)、
  [#188](https://github.com/MyBlackHole/c-logger/pull/188)：工具与 Shell 提示采用 GNU gettext
  国际化，保留机器输出和兼容接口；中文工程文档与原始历史证据分开维护。
- 生产库与私有故障/崩溃支持库、白盒回归支持库分离；测试钩子和支持归档不安装、不进入 XPack。

## 退出、错误与持久化边界

宿主先停止并 join 所有借用者，再刷新、销毁和卸载。原始句柄不支持并发销毁、
跨静态库副本使用或销毁后重试。普通 I/O 失败仍会处置实例；释放证明失败时可能保留内部
容器，不能据此重用旧指针。详见 [API](../API.md) 的入口拒绝与所有权例外。

`logger_flush_instance_status()`、`logger_destroy_status()`、`logger_shutdown_status()`
采用 0 或 -1 + errno；`logger_log_sync_status()` 采用 0 或 -errno。刷新等待既定完成水位并
同步文件，不给阻塞 I/O 提供硬超时，也不证明本地 Syslog daemon 已持久化。
部分写入后仍可能报错，应用重试可能重复；未确认尾部没有自动修复或 exactly-once 承诺。

普通 void 日志正文最多 4095 字节，超限有可见截断标记；同步状态日志超限返回
-EOVERFLOW 且不输出。截断按字节处理，不保证 UTF-8 码点完整。Syslog 为本地非阻塞
Unix 数据报，重连由后续日志触发，不提供磁盘重试队列、历史重放或远端持久确认。
Logger/Console 不是信号安全 API；支持延迟取消或入口前禁用取消，不支持一般
ASYNC+ENABLE 取消、跨调用 longjmp/pthread_exit 或宿主取消私有工作线程。

普通文件故障回归保留八个进程崩溃切点；ext4/XFS 各八个 QEMU SIGKILL、同盘重启
和恢复追加切点作为可选扩展验证，不要求每次基本 API 发布补跑 VM/掉电测试。
既有 flush/fsync 契约不变。私有崩溃程序链接静态支持库，不等于生产共享 DSO 内部注入。
这些受控虚拟存储证据不是物理断电、RAID/HBA/设备易失缓存或任意部署存储栈认证。
详见[崩溃合同](LOGGER_CRASH_CONTRACT.md)和[已知边界](KNOWN_ISSUES.md)。

## 已有验证与最终发布验收

本稿准备基线为 `55046bfe2833a13aac848f1575c763be3af868eb`，不是尚未形成的最终发布提交。
2026-10-11 只读复核该提交的适用 main 工作流全部成功，包括：

- [Xmake/生产隔离及 Sanitizer](https://github.com/MyBlackHole/c-logger/actions/runs/38129861665)；
- [GCC/Clang 工程基线](https://github.com/MyBlackHole/c-logger/actions/runs/38129861685)；
- [glibc 运行时矩阵](https://github.com/MyBlackHole/c-logger/actions/runs/38129861664)；
- [进程崩溃矩阵](https://github.com/MyBlackHole/c-logger/actions/runs/38129861687)。

前序 #168 的成功记录另包含[发布包验证](https://github.com/MyBlackHole/c-logger/actions/runs/38123451968)、
[最低内核](https://github.com/MyBlackHole/c-logger/actions/runs/38123451964)和
[VM 断电](https://github.com/MyBlackHole/c-logger/actions/runs/38123451955)。这些是已有工程证据，
不冒充上述 main 或最终发布 SHA 的执行，也不要求在每次最终 SHA 重新运行 VM。
相关源码、构建输入及验证条件相同的历史平台证据，可按[发布工程](RELEASE_ENGINEERING.md)
记录等价承接，保留旧 SHA/run ID 并注明未重新执行。适用 main 检查全绿不等于所有
发布检查均在该 SHA 跑过，包的 `SOURCE_COMMIT` 仍须如实标识实际构建源码。

最终发布须固定 SHA，核对共享/静态包、安装消费、ABI/生产隔离以及平台门禁的源码身份。
正式包在 Ubuntu 20.04/glibc 2.31/GCC 9 基线上构建，再由 Ubuntu 24.04 消费同一份包。
新系统上本地构建的产物不能据此声称支持 glibc 2.31；静态归档的最终依赖由消费方链接决定。

本次验收限定普通 Logger 基本 API 与声明的平台范围：参数/返回值、并发一致性、资源、
轮转和正常 flush/关闭语义，以及保留的进程故障回归，以可证伪测试和实际结果核对。理论推导不等于运行通过；吞吐与尾延迟
只报告明确环境下的实测值，不编造脱离硬件的阈值。本稿不声称完成特定业务部署、
物理存储认证或未给定的生产 SLO，也不把没有指定时长的长压虚构为统一发布硬门槛。

## 计划发布资产

现行 stable 发布链预期交付四份 Linux x86_64 资产：

- `prod-c-logger-2.0.0-Linux-x86_64-shared.tar.gz` 及同名 `.sha256`；
- `prod-c-logger-2.0.0-Linux-x86_64-static.tar.gz` 及同名 `.sha256`。

现行发布流程使用 `v2.0.0`、标题 `c-logger 2.0.0 — Production v2`、`prerelease=false`。
它只接受显式现存标签，锁定提交 SHA 和标签对象 ID，并在发布后重新下载四资产校验。
本稿没有创建标签、发布资产或部署；发布动作须另行批准，详见[发布工程](RELEASE_ENGINEERING.md)。
