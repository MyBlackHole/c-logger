# Logger/Console 测试与发布门禁

Xmake 是项目构建与测试的唯一权威，目标和用例在 `xmake.lua` 注册。
执行前导出 `LOGGER_PROJECT_VERSION="$(cat VERSION)"`。
下游 CMake 项目只验证安装消费，不是本项目的第二套构建系统。

## 链接边界

`logger` 是真实生产产物，故障注入和锁依赖诊断默认关闭。
`logger_test_support` 使用同组 Logger/Console 源码和测试故障钩子；
`logger_regression_support` 使用同组源码和链接期替换进行白盒验证。
后两者不安装、不进入发布包。所有配置均不编译 Audit，测试开关不能改变产品边界。

生产及测试使用 GNU C11，警告按错误处理；生产源码禁止可变长数组，并有单函数
栈预算检查。编译通过不等于生命周期、持久性或并发协议已经得到证明。

## 常用入口

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
scripts/check.sh fast
scripts/check.sh production
scripts/check.sh crash
scripts/check.sh legacy-fork
```

`fast` 运行生产库链接的公共测试；`production` 运行静态库完整核心、私有和白盒
测试。完整集合串行调度，测试内部仍有真实线程、取消、fork 和并发交错。
`legacy-fork` 单独开启兼容辅助接口；默认产品不包含该接口。
历史 unit/integration/concurrency/reliability/security/host-owned 参数运行完整集合，
不以猜测的筛选规则默默减少覆盖面。

## 安全检测

```sh
xmake f -m debug -o build-asan --build_shared=n \
  --build_tests=y --build_private_tests=y --build_regression_tests=y \
  --policies=build.sanitizer.address,build.sanitizer.undefined
xmake test -j1
```

线程检测使用同一默认静态配置，将 policies 替换为 `build.sanitizer.thread`。
Sanitizer 配置不取消内存生命周期、错误传播或普通文件后端用例。
受控 fork 辅助接口要求进程真正单线程；不能把 Sanitizer 自带后台线程忽略后
声称满足这一前提，其兼容测试由独立配置运行。

## 共享库、安装和 ABI

共享库完整测试使用 `--build_shared=y` 并开启上述三类测试。
必须覆盖真实 DSO 观察面、dlopen/dlclose、独立 SDK、全局代次与 Syslog。
白盒 `--wrap` 用例链接同源静态支持库，不能冒称拦截了真实 DSO 的内部调用。

静态、共享 XPack 产物均验证真实安装和迁移后的路径：独立 C/C++11 CMake
消费方、pkg-config、PIC 插件、精确版本/组件拒绝、公共头文件和默认值。
共享库严格匹配 SONAME 2、`LOGGER_2.0` 和 `abi/logger-2.0.symbols`。
独立 v2 C/C++ 契约检查结构布局、数值及函数类型；冻结的 v1 文件原样保留，
历史快照检查不应被修改成当前 v2 断言。

`check_production_artifact.py` 检查全部符号，包括隐藏共享符号和静态成员，
禁止 Audit、测试故障钩子或密码学存储实现混入产品。不能只隐藏导出后宣称功能移除。

## 普通文件日志的崩溃验证

`process-crash` 分组有 6 个切点：
`before_file_fsync`、`after_file_fsync`、
`file_after_archive_rename`、`file_after_archive_dirsync`、
`file_after_active_open`、`file_after_active_dirsync`。

先持久化十条带独立 ID 的基线记录，再让新进程在指定切点终止。
重启后检查每条基线记录完整、存在且只出现一次，并验证继续追加不会破坏基线。
`check_crash_matrix.py` 从实际运行日志核验精确用例集合，输出 JSON/JUnit。
未确认写入可以存在、缺失或不完整；不能把它当成承诺恢复的业务事务。

QEMU 断电框架使用同样的普通 Logger 记录，通过杀死 QEMU、同盘重启验证
ext4/XFS。完整矩阵有 7 个切点（上述 6 个加 acknowledged），PR 选择其中
同步后和归档目录同步后的 2 个切点，在两个文件系统执行。
这不是任意物理磁盘、RAID/HBA、易失缓存或其他挂载配置的掉电认证。
最低内核工作流另行验证 Linux 5.10 中的文件创建、同步和轮换行为。

## 性能和可观测性

当前 `bench_matrix` 用 Xmake 构建；历史基准仍使用其冻结提交的原构建系统，
不以新规则重解释旧数据。吞吐、时延和内存比较必须说明配置和测量条件。

保留队列满、丢弃/同步回退、缓冲耗尽、后端写入/同步失败、工作线程退出、
刷新水位和全局代次等现有故障用例。诊断数据通过真实故障核验，不由测试伪造计数。
资源转移与销毁变更必须有失败覆盖；“测试成功”不能替代锁序和最终释放关系的证明。

## 审计移除的验收边界

只删除对应已移除功能的 Audit 专用用例；混合测试中的普通 Logger 检查保留。
不能通过停止共享库测试、关闭 Sanitizer、屏蔽缺失符号或删掉文件故障测试使 CI 变绿。
合并开发 PR 不创建发布标签或发布二进制；正式发布另由人工触发完整流程。
