# 发布工程

## 1.x / 2.0 分支模型

从 2.0 开发开始，发布与开发职责分离：

```text
release/1.x
    Production 1.x 维护线

main
    2.0 development 主线
```

- `release/1.x` 从不可变 `v1.0.0` 发布提交建立；1.x 修复只在该维护线演进。
- `main` 用于 2.0 breaking redesign，不承诺 1.x source/binary ABI 兼容。
- `v1.0.0` tag/release 永久保持不可变，不允许把 tag 改指向新提交。
- 2.0 不在 `main` 维护双 runtime path 或 compatibility DSO；需要旧版行为时使用 1.x 分支/发布。
- 在 #108 完成 2.0 public API/UAPI/ABI 冻结之前，不把 `main` 产物称为 2.0 正式发布。
- 根 `VERSION` 暂不提前切成 2.0.0；正式切换 VERSION/SONAME/symbol node 属于 #108 的 ABI 冻结步骤。

2.0 的设计权威位于 `docs/v2/`；现有根目录和 `docs/` 下的 v1 架构/ABI文档继续作为 Production v1 历史与维护依据。

已发布 Production v1 为 **1.0.0**，历史 ABI 为 SONAME 1 / `LOGGER_1.0`、62符号。
当前 main 的开发 ABI 为 SONAME 2 / `LOGGER_2.0`、47符号；根 VERSION 仍待独立发布身份
迁移，不能把当前开发包作为已发布 v1 的兼容替代。

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

- 当前 main SONAME：`liblogger.so.2`；
- shared 实体文件：`liblogger.so.<VERSION>`；
- 当前 main ELF symbol version：`LOGGER_2.0`；
- 当前 public C ABI allowlist：`abi/logger-2.0.symbols`（47 项）；
- 当前安装元数据固定 `LOGGER_ABI_VERSION=2`；
- 历史 v1 opt-in fork helper 不属于当前 main；main 已移除该实现、兼容头文件与构建选项；
- production 默认隐藏可见性，并使用 version script `local: *`；
- 生产故障注入固定关闭。

`abi/logger-1.0.symbols` 仅保留已发布 v1 的历史符号清单。
新增/删除 public API 必须显式重新打开 ABI 评审，并同步更新 public header、冻结清单和语义 ABI
contract。禁止使用 glob 代替显式 allowlist。

`VERSION=1.0.0` 起，安装元数据与生成头文件的 release-candidate 标志为 false/0；
0.x 历史发布继续作为 candidate 记录保留。

### 当前 semantic ABI 契约

`tests/packaging/current_abi_contract.c` 按 C11/C++11 检查当前 Logger/Console 的结构布局、
数值常量、配置默认值与精确 C 函数类型；现有断言完整保留。当前 ELF 门禁使用
`abi/logger-2.0.symbols` 的47符号、SONAME 2与 `LOGGER_2.0`，两个门禁互相独立。
`abi/LOGGER_1_0_CONTRACT.md` 与 `abi/logger-1.0.symbols` 仅保留历史记录，旧snapshot不再
进入当前CI。此改名和清理不修改 VERSION、正式发布workflow或tag/release策略。

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

CI 使用 `EXACT` + 组件组合检查失败关闭行为；已移除的 `legacy_fork` 与未知组件均必须拒绝。

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

当前 main 保留以下独立证据链（历史 Production v1 合同见 release/1.x）：

- `crash-recovery`：8 个普通 Logger 进程 SIGKILL 用例，main/manual shared/static 配置各3次；
- `vm-powercut`：ext4 + XFS 各8个普通 Logger QEMU SIGKILL + raw-disk reboot/recovery 切断点；
  新旧矩阵与三个纯 Audit checkpoint 点的退役原因见 [crash 合同](LOGGER_CRASH_CONTRACT.md)；
- `xmake-parity` Sanitizer：ASan+UBSan 与 TSan 的完整 static suite；
- `queue-benchmark`：当前 Production v1 用 Xmake，冻结历史 baseline 用其各自 commit 的原构建定义。

这些 gate 证明指定虚拟环境中的实现行为，不替代实际服务器、电源、控制器/device volatile
cache、实际目标 挂载/存储栈 或最低 内核/glibc 验收。

## GitHub Release

`.github/workflows/release-publish.yml` 由 `main` 上的 `VERSION` / 发布 notes 变更触发。

流程：

1. 读取 `VERSION`；
2. 要求 `docs/RELEASE_NOTES_<VERSION>.md` 存在；
3. 解析 `v<VERSION>` tag/发布 状态；已有 tag 时锁定该 tag 对应 commit，重试不能改用更新后的 `main` 源码；
4. 已有 release 只有在标题、prerelease/stable 分类、四个 asset 与 SHA-256 全部正确时才视为完成并跳过；
5. 发布 缺失或不完整时，共享库/静态库 分别执行完整 Xmake 发布 suite；
6. 生成并验证 XPack + SHA-256，汇总四个 authoritative 发布资产；
7. 0.x 发布使用 prerelease；1.x+ 使用 stable Production release；已有 release 不完整时使用
   `gh release upload --clobber` 并通过 API 归一化 prerelease 标志；
8. 发布后重新下载远端四个 asset，并再次执行数量、SHA-256、release class/title 校验。

`workflow_dispatch` 因而是幂等恢复入口：完整 发布 不会重复发布，不完整 发布 会从既有 tag 的源码重建并修复。
发布新代码仍必须先递增 `VERSION`；不能把同一 tag 改指向另一份源码。

## Fork 制品边界

当前 main 不提供 legacy fork helper、兼容头文件或构建选项。所有生产制品检查都必须拒绝
`logger_fork_reinit`、进程创建符号和 `/proc/self/task` 扫描；没有 opt-in 绕过开关。
CMake/pkg-config 元数据不传播旧 helper 宏，安装检查持续验证公开头文件、当前 ABI 符号集、
共享库/静态库组件和未知/已移除组件的失败关闭行为。PID/atfork 防御和对象 census 保留。

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
