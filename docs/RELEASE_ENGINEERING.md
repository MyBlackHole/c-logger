# Release Engineering

c-logger 当前是**受控生产发布候选**，不是已经完成全部目标平台、真实掉电和安全认证的
Production v1。软件版本由仓库根 `VERSION` 唯一决定；ABI major 仍为 `0`，ELF symbol
version 仍为 `LOGGER_0.9`。

## 构建权威

项目自身只使用 Xmake：

- 最低 Xmake：2.8.5；
- CI 固定 Xmake：3.1.1；
- Linux/ELF only；
- GNU C11；
- GNU-compatible linker，需要 version-script 支持。

CMake 不再是项目 build/test/install/package 系统。发布包仍生成 CMake config metadata，
并用真实下游 CMake consumer 验证，这是兼容面而不是构建权威。

运行 Xmake 前：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

如果没有导出版本，Xmake 会拒绝生产构建，避免生成 `0.0.0` artifact。

## ABI 契约

- SONAME：`liblogger.so.0`；
- shared 实体文件：`liblogger.so.<VERSION>`；
- symbol version：`LOGGER_0.9`；
- public C symbol allowlist：`abi/logger.symbols`；
- 默认 public symbol set：62 项；
- legacy fork helper 显式开启时额外导出 `logger_fork_reinit`；
- production 默认 hidden visibility，并使用 version script `local: *`；
- production fault injection 固定关闭。

新增/删除 public API 必须同时修改 public header 与 `abi/logger.symbols`，并通过
`scripts/check_release_abi.py`。禁止用 glob 代替显式 ABI allowlist。

pre-1.0 不承诺未来版本自动 ABI 兼容；安装包的 `LoggerConfigVersion.cmake` 只对**完全相同**
版本返回 ExactVersion 成功。

## Shared build

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-shared --build_shared=y
xmake -j4 logger
```

验证：

```sh
artifact="$(find build-shared -type f -name "liblogger.so.$(cat VERSION)" -print -quit)"
python3 scripts/check_production_artifact.py "$artifact"
python3 scripts/check_release_abi.py "$artifact"
```

## Static build

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-static --build_shared=n
xmake -j4 logger
```

static production library 使用 PIC，可嵌入 SDK MODULE；fault/crash/regression support archive
不属于安装内容。

## 安装

Xmake 原生安装：

```sh
xmake install -o /opt/logger/current logger
```

安装树：

```text
<prefix>/include/logger/{logger.h,audit.h,console.h,logger_export.h,logger_version.h}
<prefix>/lib/liblogger.so.<VERSION>（或 liblogger.a）
<prefix>/lib/cmake/Logger/{LoggerConfig.cmake,LoggerConfigVersion.cmake,LoggerTargets*.cmake}
<prefix>/lib/pkgconfig/logger.pc
<prefix>/share/doc/prod_c_logger/{API.md,SECURITY.md,TESTING.md,CHANGELOG.md,docs/,examples/}
```

Xmake `-o <prefix>` 是安装根。CMake 风格 `DESTDIR` CLI 不是当前项目契约；发布 CI 对 Xmake
staged install 和最终 XPack 解包树直接做 relocation/consumer 验证，因此不需要保留旧安装
driver 作为 oracle。

不要把 shared/static 或 default/legacy 两种变体覆盖安装到同一 prefix。一个 prefix 描述一个
确定的链接/feature 组合。

## 下游 CMake / pkg-config

安装包继续提供：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

可选 component：

- `shared`
- `static`
- `legacy_fork`

CI 使用 `EXACT` + component 组合检查 fail-closed 行为。

pkg-config：

```sh
PKG_CONFIG_PATH=<prefix>/lib/pkgconfig pkg-config --cflags --libs logger
```

static consumer 使用 `pkg-config --static`。

`examples/installed_consumer/` 是发布包的独立下游工程，只能使用 installed headers/library；
CI 会检查它的 compile commands，禁止回指源码树 private include。

## XPack

权威二进制包使用内置 XPack：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-package --build_shared=y
xmake pack -f targz -o xpack-out
```

文件名：

```text
prod-c-logger-<VERSION>-Linux-x86_64-shared.tar.gz
prod-c-logger-<VERSION>-Linux-x86_64-static.tar.gz
```

每个 TGZ 同时生成同名 `.sha256`。release gate 会检查每种 kind 恰好一个包、checksum 文件名
与实际 SHA-256 一致。

XPack 是当前 CI 工具链生成的 native Linux/x86_64 候选二进制，不捆绑 glibc，也不代表任意
Linux 发行版、kernel 或 CPU 自动兼容。

## Release validation

`.github/workflows/release-validation.yml` 对 shared/static matrix 分别执行：

1. complete Xmake suite；
2. production artifact isolation；
3. shared ABI/SONAME/ELF class/machine 检查；
4. XPack TGZ + SHA-256；
5. 解包后的 installed tree；
6. relocation 到带空格前缀；
7. C/C++11 CMake consumer；
8. PIC SDK MODULE；
9. pkg-config consumer；
10. ExactVersion/components rejection；
11. frozen old-header consumer；
12. public layout/default snapshot；
13. production isolation。

测试实现见 `tests/packaging/check_install.py`。该脚本直接读取 `VERSION` 和
`abi/logger.symbols`，不维护第二份 release version/ABI 清单。

## Crash / power-cut / sanitizer / benchmark

发布候选还必须保留以下独立证据链：

- `crash-recovery`：9 个 process-crash case，shared/static 各重复 3 次；
- `vm-powercut`：10 个 QEMU SIGKILL + raw ext4 reboot/recovery cut point；
- `xmake-parity` sanitizer：ASan+UBSan 与 TSan 的完整 static suite；
- `queue-benchmark`：当前 candidate 用 Xmake，冻结历史 baseline 用其各自 commit 的原构建定义。

这些 gate 证明指定环境中的实现行为，不替代实际服务器、电源、控制器 cache、XFS 或最低
kernel/glibc 验收。

## GitHub Release

`.github/workflows/release-publish.yml` 由 `main` 上的 `VERSION` / release notes 变更触发。

流程：

1. 读取 `VERSION`；
2. 要求 `docs/RELEASE_NOTES_<VERSION>.md` 存在；
3. 如果 `v<VERSION>` 已存在则不重复发布；
4. shared/static 分别执行完整 Xmake release suite；
5. 生成并验证 XPack + SHA-256；
6. 汇总四个 release asset；
7. 创建 prerelease GitHub Release。

因此发布新代码必须先递增 `VERSION`；不能复用已经存在的 tag。

## Legacy fork helper

默认包不包含 `logger_fork_reinit`。受控兼容构建：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-legacy \
  --build_shared=n \
  --legacy_fork=y \
  --build_regression_tests=y
xmake -j4 logger fork_reinit_example
xmake test 'fork_reinit_regression/*' -j1
```

安装 legacy 变体时，CMake target/pkg-config metadata 会传播
`LOGGER_ENABLE_LEGACY_FORK_HELPER=1`，并安装 `logger_fork_compat.h`。默认包不会声明不存在
的 helper。

## 不宣称的兼容性

当前 release engineering 不证明：

- 真实服务器掉电/reset；
- RAID/HBA/NVMe/SATA volatile write cache；
- XFS 或其他目标文件系统；
- 任意网络文件系统；
- 任意多线程 fork 后复杂库调用安全；
- 32-bit；
- 所有 kernel/glibc 组合；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

平台边界见 `docs/PLATFORM_BASELINE.md` 与 `docs/KNOWN_ISSUES.md`。
