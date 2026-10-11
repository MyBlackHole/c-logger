# c-logger：高可靠 C 日志库

`main` 是 **2.0 开发主线**，使用 SONAME 2 / `LOGGER_2.0`。
版本号 `2.0.0` 是开发包身份，本次变更不创建发布标签或 GitHub Release。
已发布的 v1 契约保留在 `release/1.x`、历史标签和 `abi/logger-1.0.symbols`。

## 产品边界

提供文件、标准错误、Syslog、Console，同步和有界异步日志、文件轮换、
错误统计、刷新及生命周期管理。**不再包含审计日志功能**：没有 `audit.h`、
`audit_*` 符号、事务 BEGIN/END、哈希链、审计检查点或审计恢复引擎。
不存在通过测试开关或 `legacy_audit` 重新启用审计的生产构建。

业务事务一致性、不可变审计存储与跨系统提交由业务系统或独立服务负责。
库内不引入数据库、通用事务管理器或无需求的引用计数框架。

## 构建与安装

项目构建、测试、安装和打包以 Xmake 为唯一入口，版本由根目录 `VERSION` 提供：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-shared --build_shared=y
xmake -j4 logger
xmake install -o /opt/logger/2.0 logger
```

静态构建使用 `--build_shared=n`。不论是否启用测试，`logger` 都使用同一组
Logger/Console 源码；测试钩子只存在于不安装的私有测试支持库。

共享库为 `liblogger.so.2.0.0`，SONAME 为 `liblogger.so.2`，符号允许列表为
`abi/logger-2.0.symbols`。不能替换或冒充旧的 SONAME 1。当前 Logger/Console
配置结构体的 `version=1` 是配置协议版本，不等于 ELF ABI major。

安装包提供可迁移的 CMake config 和 pkg-config 元数据；CMake 仅用于下游消费。

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

## 宿主持有，SDK 借用

```text
宿主创建 Logger → SDK 借用 → 宿主关闭外部调用入口
→ 等待全部 SDK 调用者退出 → 刷新并检查错误
→ logger_destroy_status() 停止内部工作线程并释放资源
→ 释放宿主或卸载动态库
```

不支持业务线程继续使用对象的同时由宿主直接销毁它；宿主必须先证明外部借用结束。
SDK 不应在动态库构造/析构函数里偷偷初始化或关闭全局 Logger。
可参考 `examples/host_owned/` 的 SDK/宿主集成模板。

## 可靠性语义

入队、输出完成和持久化确认是不同的阶段。关键记录使用
`logger_log_sync_status()` 并检查返回值；刷新和销毁也有可报告错误的接口。
同步保证仍以操作系统、文件系统和设备遵守同步语义为前提。

输出失败必须可观察；历史错误不应因后续一次成功而被抹去。
无法证明线程退出或锁/对象生命周期安全时，保留依赖资源并进入失败状态，
而不是错误释放内存。日志库不承诺业务事务原子性或外部系统的恰好一次投递。

## 测试和门禁

```sh
scripts/check.sh fast
scripts/check.sh production
scripts/check.sh crash
```

核心测试、故障注入和白盒回归均不编译 Audit。GCC/Clang、ASan/UBSan、TSan、
共享库符号检查、C/C++ 安装消费测试和文件后端故障用例持续保留。
`check_production_artifact.py` 检查静态库的全部符号和共享库的符号，
即使隐藏的 Audit 对象混入产物也会失败。

进程崩溃和 QEMU 断电测试直接验证普通 Logger：十条已确认持久化记录必须各自
存活且不重复；未确认的写入允许存在、缺失或不完整。测试不是任意设备栈的物理掉电认证，
也不声称普通 Logger 会修复任意日志尾部或重放业务事务。

详见 [TESTING.md](TESTING.md)、[API.md](API.md) 和
[移除 Audit 的变更边界](docs/v2/AUDIT_REMOVAL.md)。

## 发布和兼容

合并开发 PR 不自动发布。发布工作流仅由显式手动触发；发布前仍需完整验收。
依赖 Audit 的旧用户需要保留 v1 库或迁移到独立审计系统，不能只更换库文件。
没有修改任何历史审计日志、已发布二进制或远端用户数据。

默认不构建 `logger_fork_reinit()`；受控旧场景可单独启用 `--legacy_fork=y`。
该选项与审计功能无关；原始 fork 后子进程的继承对象继续由 ECHILD 防御保护。

本项目采用 Apache License 2.0，见 [LICENSE](LICENSE)。
