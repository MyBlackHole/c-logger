# C Logger — 2.0 开发主线 / Production v1 已发布

> `main` 已进入 2.0 架构重构阶段；Production v1 由 `release/1.x` 维护，正式发布基线仍是 `v1.0.0`。\
> 根 `VERSION=2.0.0` 标识当前 ABI 2 开发产物；2.0 public API/ABI 尚未最终冻结，当前 `main` 不应被当作 2.0 正式 release 使用。


宿主显式拥有 Logger，业务 SDK 借用实例或日志回调；保留文件/标准错误/Syslog、同步/异步和 Console。
当前源码、生产目标和测试支持库均已移除 Audit/crypto；生产库没有摘要算法依赖。
当前开发 ABI 为 **SONAME 2 / `LOGGER_2.0` / 47-symbol public C ABI**。

> **开发身份不等于正式发布：** 软件包版本、生成头文件、CMake/pkg-config 与 XPack 均使用 `2.0.0`。
> 不得用当前 main 替换 SONAME 1 的正式包。2.0 最终合同与发布验收尚未完成，参见
> [迁移状态](docs/v2/AUDIT_REMOVAL.md)和[当前限制](docs/KNOWN_ISSUES.md)。

## 构建与安装

项目自身的构建、测试、安装和打包只使用 Xmake。版本号唯一来源是仓库根目录 `VERSION`：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"

# shared
xmake f -m release -o build-shared --build_shared=y
xmake -j4 logger
xmake install -o /opt/logger/"$(cat VERSION)" logger

# static
xmake f -m release -o build-static --build_shared=n
xmake -j4 logger
xmake install -o /opt/logger/"$(cat VERSION)"-static logger
```

共享产物使用 `liblogger.so.<VERSION>`，开发 ABI 的 SONAME 为 `liblogger.so.2`；静态产物为
`liblogger.a`。生产库默认关闭故障注入，并使用隐藏可见性 +
`LOGGER_2.0` ELF 符号版本。

当前开发 ABI 允许列表由 `abi/logger-2.0.symbols` 维护；变更必须经过 ABI review。
已发布 v1 的允许列表与 `release/1.x` 仅用于历史维护，不能证明当前 ABI 2 产物兼容 v1。

## 下游接入

Xmake 安装树同时提供 CMake config 与 pkg-config 元数据。这里的 CMake 是**下游消费兼容面**，
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
- PIC 静态库 嵌入 SDK 模块；
- pkg-config；
- SONAME、ELF 符号版本 与 公开符号允许列表；
- 生产产物 isolation。

`examples/installed_consumer/` 是独立下游 CMake 工程，用于验证发布包，不参与本项目自身构建。

## 测试

常用入口：

```sh
scripts/check.sh fast
scripts/check.sh production
scripts/check.sh crash
```

完整说明见 [TESTING.md](TESTING.md)。

生产、私有测试和 白盒回归 使用三条独立链接边界：

- `logger`：真实 生产产物；
- `logger_test_support`：仅测试使用，包含 故障/崩溃钩子；
- `logger_regression_support`：同源 white-box archive，用于 `--wrap` regression。

测试 hook、故障注入器 和 回归支持 不安装、不进入 XPack。

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

XPack 自动生成 TGZ 与同名 SHA-256，标题标识 `ABI 2 development`。版本号对齐和构建成功均不是正式发布授权。
普通 main push 不触发发布；正式发布要求另行批准的现存 v2 tag、最终合同冻结和精确提交的完整验收。

发布工程细节见 [docs/RELEASE_ENGINEERING.md](docs/RELEASE_ENGINEERING.md)。

## 宿主持有 集成模型

推荐模型：

```text
Host: logger_create → SDK instances borrow → stop/join SDK callers
      → destroy SDK instances → flush → logger_destroy_status → dlclose
```

业务 `.so` 不应在 构造函数/destructor 中自动初始化或关闭全局 Logger，也不应接管宿主
线程和进程生命周期。多个 SDK 可以共享宿主持有的 Logger，也可以由宿主创建独立实例。

`examples/host_owned/` 是第三方 SDK/宿主集成模板。SDK 本身不链接 Logger，宿主 adapter
把日志回调转交给自己持有的实例。

## Fork 边界

当前库不提供 `logger_fork_reinit()` 或其他进程创建 helper，也不扫描宿主线程列表。
宿主负责进程创建、线程退出与首次初始化时机。继承运行时后的 raw fork 仍由 PID/atfork
防护以 ECHILD 拒绝，child 必须 exec；移除旧 helper 不扩大 post-fork 支持范围。

## 关键文档

- [2.0 开发设计权威](docs/v2/README.md)
- [2.0 支持基线](docs/v2/SUPPORT_MATRIX.md)
- [2.0 Linux C 编码规范](docs/v2/CODING_STYLE.md)
- [2.0 执行上下文规范](docs/v2/EXECUTION_CONTEXT.md)

- [公共 API](API.md)
- [测试策略](TESTING.md)
- [发布工程](docs/RELEASE_ENGINEERING.md)
- [Xmake 迁移记录](docs/XMAKE_PARITY.md)
- [总体架构](docs/ARCHITECTURE.md)
- [Architecture Invariants](docs/ARCHITECTURE_INVARIANTS.md)
- [资源所有权](docs/RESOURCE_OWNERSHIP.md)
- [锁与锁顺序](docs/LOCKING.md)
- [并发与 publication](docs/CONCURRENCY.md)
- [生命周期](docs/LIFECYCLE.md)
- [File backend](docs/FILE_BACKEND.md)
- [Syslog backend](docs/SYSLOG_BACKEND.md)
- [可观测性与问题发现](docs/OBSERVABILITY.md)
- [平台基线](docs/PLATFORM_BASELINE.md)
- [Production v1 支持范围与发布准入](docs/V1_RELEASE_CRITERIA.md)
- [已知限制](docs/KNOWN_ISSUES.md)
- [变更清单](CHANGELOG.md)

普通日志不提供业务审计事务、哈希链或远端持久确认。旧 Audit 用户应继续使用 `release/1.x`，
或迁移至独立审计组件。历史专用源码和测试已退役，普通 Logger 的失败、锁、格式与生命周期覆盖独立保留。
文件后端进程崩溃与 QEMU/raw ext4+XFS 门禁用于验证受控存储路径；它们不等于任意服务器、
RAID/HBA/NVMe/SATA 易失缓存组合的物理断电认证。目标部署需另做存储栈 qualification。

## 许可证

本项目采用 Apache License 2.0。源码与发布包的完整许可条款见 [LICENSE](LICENSE)。
