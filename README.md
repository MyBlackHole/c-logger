# C Logger — 受控生产发布候选

仅内置 SHA-256，无 OpenSSL/SM3。宿主显式拥有 Logger，业务 SDK 借用实例或日志回调；
保留文件/标准错误/Syslog、同步/异步、Console 与完整 Audit。当前发布仍属于受控生产候选，
需要在目标平台和真实存储栈完成验收后再扩大部署范围；**不是通用 Production v1**。

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

共享产物使用 `liblogger.so.<VERSION>`，SONAME 固定为 `liblogger.so.0`；静态产物为
`liblogger.a`。生产库默认关闭 故障注入，并使用 隐藏可见性 +
`LOGGER_0.9` ELF 符号版本。

ABI 允许列表由 `abi/logger.symbols` 维护。新增 public API 时必须同步更新该清单并通过
ABI gate，不能用通配符导出。

## 下游接入

Xmake 安装树同时提供 CMake 配置 与 pkg-配置 元数据。这里的 CMake 是**下游消费兼容面**，
不是本项目的构建系统。

CMake 消费方：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

pkg-配置 消费方：

```sh
PKG_CONFIG_PATH=/opt/logger/current/lib/pkgconfig   pkg-config --cflags --libs logger
```

发布 CI 会把安装树移动到带空格的新路径后重新验证：

- C 与 C++11 消费方；
- `find_package(Logger ... EXACT CONFIG REQUIRED)`；
- 共享库/静态库/legacy 组件 失败关闭；
- PIC 静态库 嵌入 SDK 模块；
- pkg-配置；
- frozen 旧头文件 消费方；
- SONAME、ELF 符号版本 与 公开符号允许列表；
- 生产产物 隔离。

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
- `logger_regression_support`：同源 白盒归档，用于 `--wrap` 回归。

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

XPack 自动生成 TGZ 与同名 SHA-256。正式 GitHub Release 由
`.github/workflows/release-publish.yml` 生成，只发布通过 发布验证 的 XPack
共享库/静态库 产物。

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

默认生产库不编译 legacy `logger_fork_reinit()` 辅助接口。宿主负责线程退出、进程创建与
child 中的重新初始化时机。

仅在受控兼容场景显式启用：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-compat \
  --build_shared=n \
  --legacy_fork=y \
  --build_regression_tests=y
xmake -j4 fork_reinit_example
xmake test 'fork_reinit_regression/*' -j1
```

新业务 SDK 不应依赖该 辅助接口。原始 fork 防御保护 和现有 ECHILD 契约仍然适用。

## 关键文档

- [公共 API](API.md)
- [测试策略](TESTING.md)
- [发布工程](docs/RELEASE_ENGINEERING.md)
- [Xmake 迁移记录](docs/XMAKE_PARITY.md)
- [总体架构](docs/ARCHITECTURE.md)
- [Architecture Invariants](docs/ARCHITECTURE_INVARIANTS.md)
- [资源所有权](docs/RESOURCE_OWNERSHIP.md)
- [锁与锁顺序](docs/LOCKING.md)
- [并发与 发布](docs/CONCURRENCY.md)
- [生命周期](docs/LIFECYCLE.md)
- [File backend](docs/FILE_BACKEND.md)
- [Syslog backend](docs/SYSLOG_BACKEND.md)
- [可观测性与问题发现](docs/OBSERVABILITY.md)
- [内置摘要契约](docs/BUILTIN_CRYPTO.md)
- [平台基线](docs/PLATFORM_BASELINE.md)
- [Production v1 支持范围与发布准入](docs/V1_RELEASE_CRITERIA.md)
- [已知限制](docs/KNOWN_ISSUES.md)
- [变更清单](CHANGELOG.md)

Audit 是独立安全审计子系统；普通日志 回调 返回不是 Audit 持久化回执。
当前 QEMU/原始磁盘 ext4 + XFS 断电、Sanitizer、ABI、打包 和 基准测试 门禁不能替代
真实服务器、RAID/HBA/NVMe/SATA 易失缓存、实际目标存储栈以及最低支持 内核 的部署验收。
Production v1 的支持/不支持范围与剩余硬门禁见
[docs/V1_RELEASE_CRITERIA.md](docs/V1_RELEASE_CRITERIA.md)。

## 许可证

本项目采用 Apache License 2.0。源码与发布包的完整许可条款见 [LICENSE](LICENSE)。
