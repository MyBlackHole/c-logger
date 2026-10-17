<a id="c-logger--20-开发主线--production-v1-已发布"></a>

> 默认便利入口现仅借用宿主实例：使用 `logger_default_set(expected, replacement)`。
> 不再提供配置式 Global 初始化、库内销毁或空槽 stderr 回退。
> [默认借用合同](docs/v2/DEFAULT_BORROW.md)与[旧测试映射](docs/v2/DEFAULT_MIGRATION.md)区分当前能力和历史场景。


# C Logger — 2.0 稳定发布准备

> `main` 正在准备 2.0.0 生产稳定版；当前合同已整理，最终固定源码与发布包尚待验收。\
> 根 `VERSION=2.0.0`、ABI 2 不表示发布已经完成。已发布 v1 仍由 `release/1.x` 维护。


宿主显式拥有 Logger，业务 SDK 借用实例或日志回调；保留文件/标准错误/Syslog、同步/异步和 Console。
当前源码、生产目标和测试支持库均已移除 Audit/crypto；生产库没有摘要算法依赖。
当前 ABI 为 **SONAME 2 / `LOGGER_2.0` / 43 个当前公开 C ABI 符号**。

> **版本身份不等于正式发布：** 软件包版本、生成头文件、CMake/pkg-config 与 XPack 均使用 `2.0.0`。
> 不得用当前 main 替换 SONAME 1 的正式包。2.0 合同待最终验收，参见[当前 ABI 合同](abi/LOGGER_2_0_CONTRACT.md)、
> [迁移状态](docs/v2/AUDIT_REMOVAL.md)和[当前限制](docs/KNOWN_ISSUES.md)。

## 构建与安装

项目自身的构建、测试、安装和打包只使用 Xmake。版本号唯一来源是仓库根目录 `VERSION`：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"

# 共享库
xmake f -m release -o build-shared --build_shared=y
xmake -j4 logger
xmake install -o /opt/logger/"$(cat VERSION)" logger

# 静态库
xmake f -m release -o build-static --build_shared=n
xmake -j4 logger
xmake install -o /opt/logger/"$(cat VERSION)"-static logger
```

共享产物使用 `liblogger.so.<VERSION>`，ABI 的 SONAME 为 `liblogger.so.2`；静态产物为
`liblogger.a`。生产库默认关闭故障注入，并使用隐藏可见性 +
`LOGGER_2.0` ELF 符号版本。

当前 ABI 允许列表由 `abi/logger-2.0.symbols` 维护；变更必须经过 ABI 评审。
已发布 v1 的允许列表与 `release/1.x` 仅用于历史维护，不能证明当前 ABI 2 产物兼容 v1。

## 下游接入

Xmake 安装树同时提供 CMake 配置与 pkg-config 元数据。这里的 CMake 是**下游消费兼容面**，
不是本项目的构建系统。

CMake 消费方：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

pkg-config 消费方：

```sh
PKG_CONFIG_PATH=/opt/logger/current/lib/pkgconfig   pkg-config --cflags --libs logger
```

发布 CI 会把安装树移动到带空格的新路径后重新验证：

- C 与 C++11 消费方；
- `find_package(Logger ... EXACT CONFIG REQUIRED)`；
- 共享库/静态库组件及未知或已移除组件失败关闭；
- PIC 静态库嵌入 SDK 模块；
- pkg-config；
- SONAME、ELF 符号版本与公开符号允许列表；
- 生产产物隔离。

`examples/installed_consumer/` 是独立下游 CMake 工程，用于验证发布包，不参与本项目自身构建。

## 测试

常用入口：

```sh
scripts/check.sh fast
scripts/check.sh production
scripts/check.sh crash
```

完整说明见 [TESTING.md](TESTING.md)。

生产、私有测试和白盒回归使用三条独立链接边界：

- `logger`：真实生产产物；
- `logger_test_support`：仅测试使用，包含故障/崩溃钩子；
- `logger_regression_support`：同源白盒静态库，用于 `--wrap` 回归测试。

测试钩子、故障注入器和回归支持不安装、不进入 XPack。

## 打包与发布

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-release \
  --build_shared=y \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y
xmake test -j1
xmake pack -f targz -o xpack-out
```

XPack 自动生成 TGZ 与同名 SHA-256；包标题标识中性的 `ABI 2`，不包含已发布状态承诺。版本号对齐和构建成功均不是正式发布授权。
普通 main 推送不触发发布；正式发布要求另行批准的现存 v2 标签、合同确认和精确源码的完整验收。
[2.0.0 发布说明](docs/RELEASE_NOTES_2.0.0.md)记录当前范围、迁移和证据边界。

发布工程细节见 [docs/RELEASE_ENGINEERING.md](docs/RELEASE_ENGINEERING.md)。

<a id="宿主持有-集成模型"></a>

## 宿主持有集成模型

推荐模型：

```text
宿主：logger_process_init → logger_create → SDK 实例借用 → 停止 SDK 调用方并等待其退出
      → 销毁 SDK 实例 → 刷新 → logger_destroy(&owner) → dlclose
```

业务 `.so` 不应在构造函数/析构函数中自动初始化或关闭全局 Logger，也不应接管宿主
线程和进程生命周期。多个 SDK 可以共享宿主持有的 Logger，也可以由宿主创建独立实例。

`examples/host_owned/` 是第三方 SDK/宿主集成模板。SDK 本身不链接 Logger，宿主适配器
把日志回调转交给自己持有的实例。

## 显式进程初始化

宿主先调用一次 `logger_process_init()`，成功后再创建实例、配置 Console、设置
上下文或使用默认实例。普通操作不会偷偷建域，也不等待另一个初始化线程：未初始化
报告 `ENODEV`，初始化进行中报告 `EAGAIN`，失败后保留错误直到宿主显式重试。
`logger_process_init()` 成功返回 0，重复返回 `-EALREADY`，并保留调用方 `errno`。

初始化成功不等于创建了默认实例。级别设置、TLS上下文与脱敏辅助函数的剩余
返回形式仍单独迁移；纯配置和脱敏辅助函数不要求建域。完整状态/取消/多副本合同见
[显式进程准入](docs/v2/PROCESS_ADMISSION.md)。

## 实例所有权接口

`logger_create(&cfg, &owner)` 仅向空槽发布成功实例；`logger_destroy(&owner)` 在
拆除开始前消费拥有者，均返回 0/-errno 并保留 errno。旧签名与 status 销毁
已移除，详见[生命周期接口](docs/v2/LIFECYCLE_API.md)。其他 API 迁移仍独立进行。

## 刷新与重开

实例调用 `logger_flush(owner)` / `logger_reopen(owner)`，默认槽调用
`logger_default_flush()` / `logger_default_reopen()`；统一返回0/-errno并保留errno。
不再提供丢弃状态的包装或旧status符号。完成水位、文件同步、粘滞错误和默认
借用寿命不变，见[控制操作合同](docs/v2/CONTROL_API.md)。

## Console 状态接口

Console 配置、输出及TTY查询统一返回0/-errno并保留errno。TTY使用输出槽，
区分正常的非终端与查询错误；Console不拥有默认Logger。旧签名和errno错误风格
不保留，见[Console合同](docs/v2/CONSOLE_API.md)。

## 记录与查询状态

四个记录入口均返回0/-errno并保留errno，日志宏传回该状态。异步成功表示入队，
容量DROP返回-EAGAIN；发布后通知失败或输出后的收尾错误不表示可以直接重试。
普通可见截断和严格同步的溢出拒绝保持区别，见[记录合同](docs/v2/RECORD_API.md)。

十二个状态/指标查询统一为状态加输出槽，完整成功才写出，失败保留原值和errno。
state/dropped查询不再直接返回数据；旧void指标的失败清零规则已替换。无锁
诊断仍允许观察存活的故障/停止实例，默认查询保留完整代次借用，见
[查询合同](docs/v2/QUERY_API.md)。

## fork 边界

#218 的当前开发实现支持**有效 fork 快照下的子方显式运行域重建**：父进程
继续原实例；子进程先 `logger_process_init()`，再用自己的配置创建实例。
实例、队列、工作区及 Global/Console 的锁位于独立 `MADV_DONTFORK` 映射；
继承 fd 由既有快照关闭，不重复输出父队列，不获取旧实例锁或 join 父方线程。

所有父方实例指针在子方失效，不能普通销毁或在重初始化后继续使用。原始系统调用、
注册窗口或其他无法获得有效快照的路径仍拒绝。库不代替宿主 fork，不强制使用代理。

这是 Linux/glibc 候选实现：已加入父子实际输出、映射隔离和失败回滚测试，但宿主
外部时区/区域设置/自定义格式化或分配器等状态不受库内协调锁保护；完整平台、
性能及新 API 迁移验收未完成。**本地手工测试不等于 Xmake/CI 或生产发布通过。**
详见 [运行域与验收边界](docs/v2/FORK_RUNTIME.md)。

## 关键文档

- [项目语言约定](docs/PROJECT_LANGUAGE.md)
- [项目协作约定](AGENTS.md)

- [2.0 设计权威](docs/v2/README.md)
- [2.0 支持基线](docs/v2/SUPPORT_MATRIX.md)
- [2.0 Linux C 编码规范](docs/v2/CODING_STYLE.md)
- [2.0 执行上下文规范](docs/v2/EXECUTION_CONTEXT.md)

- [公共 API](API.md)
- [测试策略](TESTING.md)
- [发布工程](docs/RELEASE_ENGINEERING.md)
- [架构不变量](docs/ARCHITECTURE_INVARIANTS.md)
- [资源所有权](docs/RESOURCE_OWNERSHIP.md)
- [锁与锁顺序](docs/LOCKING.md)
- [并发与发布](docs/CONCURRENCY.md)
- [生命周期](docs/LIFECYCLE.md)
- [文件后端](docs/FILE_BACKEND.md)
- [Syslog 后端](docs/SYSLOG_BACKEND.md)
- [可观测性与问题发现](docs/OBSERVABILITY.md)
- [平台基线](docs/PLATFORM_BASELINE.md)
- [已知限制](docs/KNOWN_ISSUES.md)
- [变更清单](CHANGELOG.md)

普通日志不提供业务审计事务、哈希链或远端持久确认。旧 Audit 用户应继续使用 `release/1.x`，
或迁移至独立审计组件。历史专用源码和测试已退役，普通 Logger 的失败、锁、格式与生命周期覆盖独立保留。
基本 API 验收保留文件后端进程 SIGKILL 故障回归；当前不维护 VM/断电测试，
其专用测试程序、脚本及 CI 已移除。既有 flush/fsync 契约不变。进程测试不等于任意服务器、RAID/HBA/NVMe/SATA
易失缓存组合的物理断电认证；有此额外要求的部署需另做存储栈专项验证。

## 许可证

本项目采用 Apache License 2.0。源码与发布包的完整许可条款见 [LICENSE](LICENSE)。
