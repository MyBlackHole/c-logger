# C Logger — 受控生产发布候选

仅内置 SHA-256，无 OpenSSL/SM3。宿主显式拥有 Logger，业务 SDK 借用实例或日志回调；
保留文件/stderr/Syslog、同步/异步、Console 与完整 Audit。当前发布仍属于受控生产候选，
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
`liblogger.a`。生产库默认关闭 fault injection，并使用 hidden visibility +
`LOGGER_0.9` ELF symbol version。

ABI 允许列表由 `abi/logger.symbols` 维护。新增 public API 时必须同步更新该清单并通过
ABI gate，不能用通配符导出。

## 下游接入

Xmake 安装树同时提供 CMake config 与 pkg-config metadata。这里的 CMake 是**下游消费兼容面**，
不是本项目的构建系统。

CMake consumer：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

pkg-config consumer：

```sh
PKG_CONFIG_PATH=/opt/logger/current/lib/pkgconfig   pkg-config --cflags --libs logger
```

发布 CI 会把安装树移动到带空格的新路径后重新验证：

- C 与 C++11 consumer；
- `find_package(Logger ... EXACT CONFIG REQUIRED)`；
- shared/static/legacy component fail-closed；
- PIC static library 嵌入 SDK MODULE；
- pkg-config；
- frozen old-header consumer；
- SONAME、ELF symbol version 与 public symbol allowlist；
- production artifact isolation。

`examples/installed_consumer/` 是独立下游 CMake 工程，用于验证发布包，不参与本项目自身构建。

## 测试

常用入口：

```sh
scripts/check.sh fast
scripts/check.sh production
scripts/check.sh crash
```

完整说明见 [TESTING.md](TESTING.md)。

生产、私有测试和 white-box regression 使用三条独立链接边界：

- `logger`：真实 production artifact；
- `logger_test_support`：仅测试使用，包含 fault/crash hooks；
- `logger_regression_support`：same-source white-box archive，用于 `--wrap` regression。

测试 hook、fault injector 和 regression support 不安装、不进入 XPack。

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
`.github/workflows/release-publish.yml` 生成，只发布通过 release validation 的 XPack
shared/static 产物。

发布工程细节见 [docs/RELEASE_ENGINEERING.md](docs/RELEASE_ENGINEERING.md)。

## Host-owned 集成模型

推荐模型：

```text
Host: logger_create → SDK instances borrow → stop/join SDK callers
      → destroy SDK instances → flush → logger_destroy_status → dlclose
```

业务 `.so` 不应在 constructor/destructor 中自动初始化或关闭全局 Logger，也不应接管宿主
线程和进程生命周期。多个 SDK 可以共享宿主持有的 Logger，也可以由宿主创建独立实例。

`examples/host_owned/` 是第三方 SDK/宿主集成模板。SDK 本身不链接 Logger，宿主 adapter
把日志回调转交给自己持有的实例。

## Fork 边界

默认生产库不编译 legacy `logger_fork_reinit()` helper。宿主负责线程退出、进程创建与
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

新业务 SDK 不应依赖该 helper。raw-fork defensive guard 和现有 ECHILD 契约仍然适用。

## 关键文档

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
- [内置摘要契约](docs/BUILTIN_CRYPTO.md)
- [平台基线](docs/PLATFORM_BASELINE.md)
- [已知限制](docs/KNOWN_ISSUES.md)
- [变更清单](CHANGELOG.md)

Audit 是独立安全审计子系统；普通日志 callback 返回不是 Audit 持久化回执。
当前 QEMU/raw-disk ext4 + XFS power-cut、sanitizer、ABI、packaging 和 benchmark 门禁不能替代
真实服务器、RAID/HBA/NVMe/SATA volatile cache、实际目标存储栈以及最低支持 kernel/glibc
的部署验收。
