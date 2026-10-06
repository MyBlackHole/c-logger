# 发布工程

c-logger 当前仍是**受控生产发布候选**，不是已经完成全部目标平台、真实掉电和安全认证的
Production 1.0。软件版本由仓库根 `VERSION` 唯一决定；Production v1 ABI identity 已提前
冻结为 SONAME 1 / `LOGGER_1.0`，但当前软件版本仍为 0.9.6 candidate。ABI 冻结不等于 1.0 发布。

## 构建权威

项目自身只使用 Xmake：

- 最低 Xmake：2.8.5；
- CI 固定 Xmake：3.1.1；
- 仅 Linux/ELF；
- GNU C11；
- GNU 兼容链接器，需要 版本脚本 支持。

CMake 不再是项目 构建/测试/安装/打包 系统。发布包仍生成 CMake config 元数据，
并用真实下游 CMake 消费方 验证，这是兼容面而不是构建权威。

运行 Xmake 前：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

如果没有导出版本，Xmake 会拒绝生产构建，避免生成 `0.0.0` artifact。

## ABI 契约

- SONAME：`liblogger.so.1`；
- shared 实体文件：`liblogger.so.<VERSION>`；
- 默认 ELF symbol version：`LOGGER_1.0`；
- 唯一 Production v1 public C ABI allowlist：`abi/logger-1.0.symbols`（62 项）；
- 安装元数据固定 `LOGGER_ABI_VERSION=1`；
- 旧版 fork 辅助接口仅在显式 compatibility build 中额外导出 `logger_fork_reinit`；
- production 默认隐藏可见性，并使用 version script `local: *`；
- 生产故障注入固定关闭。

`abi/logger-1.0.symbols` 是 v1 ABI 的唯一权威符号清单，不再保留 pre-v1 active manifest。
新增/删除 public API 必须显式重新打开 ABI 评审，并同步更新 public header、冻结清单和语义 ABI
contract。禁止使用 glob 代替显式 allowlist。

当前 `VERSION=0.9.6` 只表示软件发布仍处于 candidate 阶段；它不会改变已经冻结的 ABI major 1。
正式 `VERSION=1.0.0` 只能在其余 Production v1 release hard gate 全部关闭后单独完成。

### v1 semantic ABI 契约

`abi/LOGGER_1_0_CONTRACT.md` 与 `tests/packaging/v1_abi_contract.c` 冻结初始 v1 的语义 ABI：

- 所有现有 public struct 的 size/alignment/全部 field offset；
- public enum 与数值宏；
- 62 个默认 production public function 的精确 C type；
- Logger/Audit/Console 配置版本与默认值；
- 1.x 增量式-only 规则和需要新 ABI major 的破坏性变化。

该 契约 同时按 C11/C++11 编译运行，并进入 `xmake-parity` 和 共享库/静态库
`release-validation`。ELF SONAME/symbol-version gate 与 semantic 契约 是两层独立门禁。

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

static production library 使用 PIC，可嵌入 SDK 模块；fault/crash/回归支持 archive
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
<prefix>/share/doc/prod_c_logger/{LICENSE,API.md,SECURITY.md,TESTING.md,CHANGELOG.md,docs/,examples/}
```

Xmake `-o <prefix>` 是安装根。CMake 风格 `DESTDIR` CLI 不是当前项目契约；发布 CI 对 Xmake
staged install 和最终 XPack 解包树直接做 relocation/消费方 验证，因此不需要保留旧安装
driver 作为 oracle。

不要把 共享库/静态库 或 default/legacy 两种变体覆盖安装到同一 prefix。一个 prefix 描述一个
确定的链接/feature 组合。

## 下游 CMake / pkg-config

安装包继续提供：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

可选 组件：

- `shared`
- `static`
- `legacy_fork`

CI 使用 `EXACT` + 组件 组合检查 失败关闭 行为。

pkg-config：

```sh
PKG_CONFIG_PATH=<prefix>/lib/pkgconfig pkg-config --cflags --libs logger
```

static 消费方 使用 `pkg-config --static`。

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

每个 TGZ 同时生成同名 `.sha256`。发布门禁 会检查每种 kind 恰好一个包、checksum 文件名
与实际 SHA-256 一致。

XPack 是当前 CI 工具链生成的 native Linux/x86_64 候选二进制，不捆绑 glibc，也不代表任意
Linux 发行版、内核 或 CPU 自动兼容。

## Release validation

`.github/workflows/release-validation.yml` 对 共享库/静态库 matrix 分别执行：

1. complete Xmake suite；
2. 生产产物 isolation；
3. shared ABI/SONAME/ELF class/machine 检查；
4. XPack TGZ + SHA-256；
5. 解包后的 installed tree；
6. relocation 到带空格前缀；
7. C/C++11 CMake 消费方；
8. PIC SDK 模块；
9. pkg-config 消费方；
10. 精确版本/组件 rejection；
11. current C/C++ public layout/default 快照；
12. production isolation。

测试实现见 `tests/packaging/check_install.py`。该脚本直接读取 `VERSION` 和
`abi/logger-1.0.symbols`，不维护第二份发布 version/ABI 清单。

## Crash / 断电 / Sanitizer / 基准测试

发布候选还必须保留以下独立证据链：

- `crash-recovery`：9 个 进程崩溃用例，共享库/静态库 各重复 3 次；
- `vm-powercut`：ext4 + XFS 各 10 个 QEMU SIGKILL + raw-disk reboot/recovery 切断点；
- `xmake-parity` Sanitizer：ASan+UBSan 与 TSan 的完整 static suite；
- `queue-benchmark`：当前 candidate 用 Xmake，冻结历史 baseline 用其各自 commit 的原构建定义。

这些 gate 证明指定虚拟环境中的实现行为，不替代实际服务器、电源、控制器/device volatile
cache、实际目标 挂载/存储栈 或最低 内核/glibc 验收。

## GitHub Release

`.github/workflows/release-publish.yml` 由 `main` 上的 `VERSION` / 发布 notes 变更触发。

流程：

1. 读取 `VERSION`；
2. 要求 `docs/RELEASE_NOTES_<VERSION>.md` 存在；
3. 解析 `v<VERSION>` tag/发布 状态；已有 tag 时锁定该 tag 对应 commit，重试不能改用更新后的 `main` 源码；
4. 已有 发布 只有在 pre发布 元数据正确、四个 asset 齐全且包内 SHA-256 校验全部通过时才视为完成并跳过；
5. 发布 缺失或不完整时，共享库/静态库 分别执行完整 Xmake 发布 suite；
6. 生成并验证 XPack + SHA-256，汇总四个 authoritative 发布资产；
7. 发布 不存在时创建 pre发布；发布 已存在但不完整时使用 `gh release upload --clobber` 修复同名资产；
8. 发布后重新下载远端四个 asset，并再次执行数量、SHA-256、pre发布/title 校验。

`workflow_dispatch` 因而是幂等恢复入口：完整 发布 不会重复发布，不完整 发布 会从既有 tag 的源码重建并修复。
发布新代码仍必须先递增 `VERSION`；不能把同一 tag 改指向另一份源码。

## Legacy fork 辅助接口

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

安装 legacy 变体时，CMake target/pkg-config 元数据 会传播
`LOGGER_ENABLE_LEGACY_FORK_HELPER=1`，并安装 `logger_fork_compat.h`。默认包不会声明不存在
的 辅助接口。

## 不宣称的兼容性

当前 发布 engineering 不证明：

- 真实服务器掉电/reset；
- RAID/HBA/NVMe/SATA volatile write cache；
- 真实目标 ext4/XFS 存储栈的物理掉电语义；
- v1 明确 不支持 的网络/分布式文件系统；
- 任意多线程 fork 后复杂库调用安全；
- 32-bit；
- 所有 内核/glibc 组合；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

平台边界见 `docs/PLATFORM_BASELINE.md`、`docs/V1_RELEASE_CRITERIA.md` 与
`docs/KNOWN_ISSUES.md`。
