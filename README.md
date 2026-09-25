# C Logger 0.9.0 — 完整功能发布工程候选

仅内置 SHA-256，无 OpenSSL/SM3。宿主显式拥有 Logger，业务 SDK 借用实例或日志回调；
保留文件/stderr/Syslog、同步/异步、Console、完整 Audit。**不是已完成部署验收的 Production v1。**

## 安装及独立接入

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build build --target check -j4
cmake --install build --prefix /opt/logger/0.9.0
```

```cmake
find_package(Logger 0.9.0 EXACT CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

共享产物 `liblogger.so.0.9.0` / SONAME `liblogger.so.0`；静态版本使用独立 build/prefix。
生成的安装包仅含生产库、公开头、CMake/pkg-config 元数据和集成文档，不包含测试钩子。
`examples/installed_consumer/` 可以直接从安装树用 C/C++11 编译运行。

- [总体架构与 Architecture Invariants](docs/ARCHITECTURE.md)
- [发布工程、版本与兼容边界](docs/RELEASE_ENGINEERING.md)
- [实际平台构建基线与门禁](docs/PLATFORM_BASELINE.md)
- [代码与注释规范](docs/CODING_STYLE.md)
- [资源所有权规范](docs/RESOURCE_OWNERSHIP.md)
- [资源 ownership 审计矩阵](docs/RESOURCE_OWNERSHIP_MATRIX.md)
- [引用计数使用规范](docs/REFCOUNTING.md)
- [Linux-style 自动清理规范](docs/RESOURCE_CLEANUP.md)
- [锁与锁顺序](docs/LOCKING.md)
- [Lock Matrix](docs/LOCK_MATRIX.md)
- [并发 / atomic / publication](docs/CONCURRENCY.md)
- [Queue compact storage / spill pool / benchmark](docs/QUEUE_STORAGE.md)
- [Queue hot-path 验证](validation/QUEUE_HOTPATH.md)
- [错误处理规范](docs/ERROR_HANDLING.md)
- [生命周期与 publish/destroy](docs/LIFECYCLE.md)
- [本轮验证报告](validation/PACKAGING_RESULTS.md)
- [变更清单](CHANGELOG.md)

以下宿主/运行时契约继续适用；历史构建状态由上方最新发布工程文档取代。

---

> 本版已移除 OpenSSL 使用：构建、生产库和当前测试均不再链接 libcrypto/libssl。
> 只提供内置 SHA-256，Audit 功能保留；SM3 已删除。构建与历史兼容说明见
> [内置摘要契约](docs/BUILTIN_CRYPTO.md)，本轮结果见
> [SHA-256 收敛验证](validation/SHA256_ONLY_RESULTS.md)。历史验证数据不作当前依赖声明。

> 当前 Syslog 已改为非阻塞/有界尝试，实例配置、重连和指标见
> [docs/SYSLOG_BACKEND.md](docs/SYSLOG_BACKEND.md)。测试结果见
> [validation/SYSLOG_RESULTS.md](validation/SYSLOG_RESULTS.md)。默认仍为完整功能开发版，
> 未宣称 Production v1；旧轮次说明中的阻塞 Syslog 语义不再适用。

# C Logger — host-owned integration development version

**尚未标记为 Production v1。** 本轮收敛第三方 `.so` 集成，不要求宿主采用特定进程模型。

## 默认模型

宿主显式创建、配置和销毁 `logger_t`；业务库只借用实例或调用宿主提供的日志回调。
不在业务 `.so` 的 constructor/destructor 中自动初始化/关闭全局 Logger，不接管宿主线程或进程。

```text
Host: logger_create → SDK instances borrow → stop/join SDK callers
      → destroy SDK instances → flush → logger_destroy_status → dlclose
```

多个 SDK 可以借用同一个 Logger，也可以由宿主分别创建 Logger。异步模式的 worker、queue
仅在显式 `logger_create()` 时创建；`LOGGER_DEFAULT_CONFIG()` 的历史 async 默认值仍为 1，
没有静默改变，宿主应明确设置 `cfg.async_mode`。加载 callback-only SDK 不会创建 Logger。

## 构建和示例

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build build --target check -j4
build/host_owned_example "$(pwd)/build/libexample_sdk.so" ./host.log
```

`examples/host_owned/` 是可编译的第三方 SDK/宿主模板，不是已经改造了用户的外部业务工程。
`libexample_sdk.so` 不链接 Logger，宿主的 adapter 将同步日志回调转交给自己持有的实例。
`BUILD_SHARED_LIBS=OFF` 保留静态库构建。已增加候选版本 install/export/SONAME，详见 RELEASE_ENGINEERING.md；跨平台和正式 v1 ABI 仍需验收。

## Fork 边界

默认生产库不编译 `logger_fork_reinit()`，不调用 fork，不扫描 `/proc/self/task`。
宿主负责进程创建、线程退出、实例初始化时机。已有 raw-fork 继承状态防护仍保留；
独立实例 **不是** 任意多线程 fork 后调用复杂库的安全豁免，也没有解除现有 ECHILD 规则。

旧受控 wrapper 仅兼容构建提供：

```sh
cmake -S . -B build-compat -DLOGGER_ENABLE_LEGACY_FORK_HELPER=ON
```

CMake target 会传递该宏；手动集成时包含 `logger_fork_compat.h` 并链接兼容产物。
不要在新业务 SDK 中依赖它。全局 `LOG_*` 便利层保留给自有应用，SDK 默认不使用它。

## 文档

- [第三方集成与卸载](docs/HOST_OWNED.md)
- [公共 API 边界](API.md)
- [测试说明](TESTING.md)
- [已知发布阻断项](docs/KNOWN_ISSUES.md)
- [当前验证](validation/SHA256_ONLY_RESULTS.md)
- [宿主集成历史验证](validation/HOST_OWNED_RESULTS.md)
- 旧逐轮说明归档：[历史 README](docs/HISTORY_README.md)、[历史 API](docs/HISTORY_API.md)。

Audit 仍是独立的安全审计子系统，普通日志 callback 返回不是审计持久化回执。

## File backend hardening revision

The latest contract is in `docs/FILE_BACKEND.md`. File ownership, no-clobber
timestamp rotation, directory binding and transactional-fd reopen supersede
earlier descriptions of these behaviors. Multiple SDKs may share a host-owned
logger; multiple logger instances no longer independently own the same regular
file target. This remains a development version; the remaining limitations in
`docs/KNOWN_ISSUES.md` are not waived. The external crypto backend is removed.

## Global 生命周期加固

全局写入、flush、reopen、level 和 metrics 现在共享代次/阶段准入；关闭后新 reader
不延长旧实例生命，延迟调用不会误入新实例。新增 `logger_shutdown_status()`，取消
延迟到资源释放后生效。见 [契约](docs/GLOBAL_LIFECYCLE.md) 和
[本轮验证报告](validation/GLOBAL_LIFECYCLE_RESULTS.md)。这不改变宿主拥有显式实例
的推荐集成方式；显式 API/Console 的后续取消修复见下节。

## Explicit-instance and Console cancellation hardening

Explicit Logger and Console now use a private cancellation/reentry scope.
The normal host-owned integration model is unchanged. Important constructor,
cleanup, callback and stdio error boundaries are documented in
`docs/EXPLICIT_CONSOLE_SCOPE.md`; current evidence is in
`validation/EXPLICIT_CONSOLE_RESULTS.md`. No throughput claim is made for this change.
