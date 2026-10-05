# 发布工程

c-logger 当前是**受控生产发布候选**，不是已经完成全部目标平台、真实掉电和安全认证的
生产版 v1。软件版本由仓库根 `VERSION` 唯一决定；ABI 主版本 仍为 `0`，ELF symbol
version 仍为 `LOGGER_0.9`。

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

- SONAME：`liblogger.so.0`；
- 共享库 实体文件：`liblogger.so.<VERSION>`；
- 符号版本：`LOGGER_0.9`；
- 当前公开 C 符号允许列表：`abi/logger.symbols`；
- frozen 生产版 v1 快照：`abi/logger-1.0.symbols`（66 项，加固 阶段禁止漂移）；
- 0.9.6 候选 公开符号集：66 项；相对 0.9.5 的 62 项仅新增两个 诊断访问接口 与两个 文件指标 访问接口；
- 旧版 fork 辅助接口 显式开启时额外导出 `logger_fork_reinit`；
- 生产 默认 隐藏可见性，并使用 版本脚本 `local: *`；
- 生产故障注入 固定关闭。

新增/删除 public API 必须同时修改 公开头文件 与 `abi/logger.symbols`，并通过
`scripts/check_release_abi.py`。进入 v1 加固 后，`scripts/check_v1_abi_snapshot.py` 还要求
活动文件 manifest 与 冻结的 v1 快照 完全一致；任何 公开接口面 变化必须先显式重新打开
ABI 评审，而不能顺手修改 快照。禁止用 glob 代替显式 ABI 允许列表。

pre-1.0 不承诺未来版本自动 ABI 兼容；安装包的 `LoggerConfigVersion.cmake` 只对**完全相同**
版本返回 精确版本 成功。

生产版 v1 的长期支持范围和 ABI 升级目标不由本文件隐式推断，统一以
[ V1_RELEASE_CRITERIA.md ](V1_RELEASE_CRITERIA.md) 为准。当前目标是在正式 v1 前完成
`liblogger.so.1` / `LOGGER_1.0` / frozen ABI 快照 的独立迁移和 消费方 验证。

### 非默认 v1 ABI 预览

v1 加固 使用显式的 `--v1_abi_preview=y` 配置验证未来 ABI identity，而不改变当前
0.9.6 默认产物：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o v1-abi-build \
  --build_shared=y \
  --build_tests=y \
  --v1_abi_preview=y
xmake -j4 logger
xmake test -j2

artifact="$(find v1-abi-build -type f -name "liblogger.so.$(cat VERSION)" -print -quit)"
python3 scripts/check_release_abi.py "$artifact" \
  --manifest abi/logger-1.0.symbols \
  --abi-version 1 \
  --symbol-version LOGGER_1.0
```

预览 的目标契约是 SONAME `liblogger.so.1`、符号版本 `LOGGER_1.0`、冻结的
66-symbol v1 快照，以及安装 元数据 中 `LOGGER_ABI_VERSION=1`。CI 同时运行
共享库/静态库 installed 消费方。

预览 **不是正式 v1 发布**：`VERSION` 仍为 0.9.6，默认构建仍是 ABI 0；预览
XPack 也使用独立的 `prod-c-logger-v1-abi-preview-...` 名称，不能被正式 发布资产
检查接受。最终把 ABI 1 切为默认值必须与正式 v1 版本迁移一起完成。

### v1 semantic ABI 契约

`abi/LOGGER_1_0_CONTRACT.md` 与 `tests/packaging/v1_abi_contract.c` 冻结初始 v1 的语义 ABI：

- 所有现有 public struct 的 size/alignment/全部 field offset；
- public enum 与数值宏；
- 66 个默认 生产 public function 的精确 C type；
- Logger/Audit/Console 配置版本与默认值；
- 1.x 增量式-only 规则和需要新 ABI 主版本 的破坏性变化。

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

静态库 生产 library 使用 PIC，可嵌入 SDK 模块；fault/crash/回归支持 archive
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

静态库 消费方 使用 `pkg-config --static`。

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

## 发布构建 validation

`.github/workflows/release-validation.yml` 对 共享库/静态库 matrix 分别执行：

1. complete Xmake suite；
2. 生产产物 isolation；
3. 共享库 ABI/SONAME/ELF class/machine 检查；
4. XPack TGZ + SHA-256；
5. 解包后的 installed tree；
6. relocation 到带空格前缀；
7. C/C++11 CMake 消费方；
8. PIC SDK 模块；
9. pkg-config 消费方；
10. 精确版本/组件s rejection；
11. frozen 旧头文件 消费方；
12. public layout/default 快照；
13. 生产 isolation。

测试实现见 `tests/packaging/check_install.py`。该脚本直接读取 `VERSION` 和
`abi/logger.symbols`，不维护第二份 发布 version/ABI 清单。

## Crash / 断电 / Sanitizer / 基准测试

发布候选还必须保留以下独立证据链：

- `crash-recovery`：9 个 进程崩溃用例，共享库/静态库 各重复 3 次；
- `vm-powercut`：ext4 + XFS 各 10 个 QEMU SIGKILL + 原始磁盘 reboot/recovery 切断点；
- `xmake-parity` Sanitizer：ASan+UBSan 与 TSan 的完整 静态库 suite；
- `queue-benchmark`：当前 candidate 用 Xmake，冻结历史 baseline 用其各自 提交 的原构建定义。

这些 gate 证明指定虚拟环境中的实现行为，不替代实际服务器、电源、控制器/device volatile
cache、实际目标 挂载/存储栈 或最低 内核/glibc 验收。

## GitHub 发布构建

`.github/workflows/release-publish.yml` 由 `main` 上的 `VERSION` / 发布 notes 变更触发。

流程：

1. 读取 `VERSION`；
2. 要求 `docs/RELEASE_NOTES_<VERSION>.md` 存在；
3. 解析 `v<VERSION>` tag/发布 状态；已有 tag 时锁定该 tag 对应 提交，重试不能改用更新后的 `main` 源码；
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

安装 legacy 变体时，CMake 目标/pkg-config 元数据 会传播
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
