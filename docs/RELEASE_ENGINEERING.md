# 发布工程

## 1.x / 2.0 分支模型

从 2.0 开发开始，发布与开发职责分离：

```text
release/1.x
    生产版 1.x 维护线

main
    2.0 稳定发布准备主线
```

- `release/1.x` 从不可变 `v1.0.0` 发布提交建立；1.x 修复只在该维护线演进。
- `main` 用于 2.0 不兼容重新设计，不承诺 1.x 源码/二进制 ABI 兼容。
- `v1.0.0` 标签/发布永久保持不可变，不允许把标签改指向新提交。
- 2.0 不在 `main` 维护双运行时路径或兼容 DSO；需要旧版行为时使用 1.x 分支/发布。
- 当前[公开 ABI 合同](../abi/LOGGER_2_0_CONTRACT.md)依据现有可执行快照整理，须与最终源码一并验收；不能把准备产物称为已经发布。
- 根 `VERSION=2.0.0` 与 SONAME 2 / `LOGGER_2.0` 对齐版本身份；版本号迁移不代表最终验收完成或正式发布批准。

2.0 的设计权威位于 `docs/v2/`；现有根目录和 `docs/` 下的 v1 架构/ABI文档继续作为生产版 v1 历史与维护依据。

已发布生产版 v1 为 **1.0.0**，历史 ABI 为 SONAME 1 / `LOGGER_1.0`、62符号。
当前 main 的版本身份为 VERSION 2.0.0、SONAME 2 / `LOGGER_2.0`、47符号；XPack 标题与
pkg-config描述均标识中性的 `ABI 2`，不代表已验收或已发布；不能作为已发布 v1 的兼容替代。

## 构建权威

项目自身只使用 Xmake：

- 最低 Xmake：2.8.5；
- CI 固定 Xmake：3.1.1；
- 仅 Linux/ELF；
- GNU C11；
- GNU 兼容链接器，需要版本脚本支持。

CMake 不再是项目构建/测试/安装/打包系统。发布包仍生成 CMake 配置元数据，
并用真实下游 CMake 消费方验证，这是兼容面而不是构建权威。

运行 Xmake 前：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

Xmake 在配置阶段校验根 `VERSION` 的格式，并要求导出的版本与其精确一致；缺失、非法
或旧的环境变量均失败关闭，避免生成 `0.0.0` 或版本身份错误的制品。

## ABI 契约

- 当前 main SONAME：`liblogger.so.2`；
- 共享库实体文件：`liblogger.so.<VERSION>`；
- 当前 main ELF 符号版本：`LOGGER_2.0`；
- 当前公开 C ABI 允许列表：`abi/logger-2.0.symbols`（47 项）；
- 当前安装元数据固定 `LOGGER_ABI_VERSION=2`；
- 历史 v1 显式启用的 fork 辅助接口不属于当前 main；main 已移除该实现、兼容头文件与构建选项；
- 生产环境默认隐藏可见性，并使用版本脚本 `local: *`；
- 生产故障注入固定关闭。

`abi/logger-1.0.symbols` 仅保留已发布 v1 的历史符号清单。
新增/删除公开 API 必须显式重新打开 ABI 评审，并同步更新公开头文件、冻结清单和语义 ABI
契约。禁止使用 glob 代替显式允许列表。

安装元数据与生成头文件的发布候选标志沿用主版本号为 0 时 true/1 的历史规则；
当前 2.0.0 为 false/0。该标志只区分历史 0.x 候选版本，不证明 API/ABI 已冻结、验证通过
或已获正式发布授权；0.x 历史发布继续作为候选版本记录保留。

<a id="当前-semantic-abi-契约"></a>

### 当前语义 ABI 契约

`tests/packaging/current_abi_contract.c` 按 C11/C++11 检查当前 Logger/Console 的结构布局、
数值常量、配置默认值与精确 C 函数类型；现有断言完整保留。当前 ELF 门禁使用
`abi/logger-2.0.symbols` 的47符号、SONAME 2与 `LOGGER_2.0`，两个门禁互相独立。
`abi/LOGGER_1_0_CONTRACT.md` 与 `abi/logger-1.0.symbols` 仅保留历史记录，旧快照不再
进入当前CI。语义 ABI 门禁独立于软件包版本身份与正式标签/发布授权。

<a id="shared-build"></a>

## 共享库构建

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

<a id="static-build"></a>

## 静态库构建

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-static --build_shared=n
xmake -j4 logger
```

静态生产库使用 PIC，可嵌入 SDK 模块；故障/崩溃/回归支持归档
不属于安装内容。

## 安装

Xmake 原生安装：

```sh
xmake install -o /opt/logger/current logger
```

安装树：

```text
<prefix>/include/logger/{logger.h,console.h,logger_export.h,logger_version.h}
<prefix>/lib/liblogger.so.<VERSION>（或 liblogger.a）
<prefix>/lib/cmake/Logger/{LoggerConfig.cmake,LoggerConfigVersion.cmake,LoggerTargets*.cmake}
<prefix>/lib/pkgconfig/logger.pc
<prefix>/share/doc/prod_c_logger/{LICENSE,README.md,API.md,SECURITY.md,TESTING.md,CHANGELOG.md,docs/,validation/,examples/}
```

Xmake `-o <prefix>` 是安装根。CMake 风格 `DESTDIR` CLI 不是当前项目契约；发布 CI 对 Xmake
暂存安装和最终 XPack 解包树直接做重定位/消费方验证，因此不需要保留旧安装
驱动脚本作为判定依据。

文档由 `docs/INSTALL_MANIFEST.txt` 显式列出，保留 `docs/v2/` 和当前迁移说明依赖的相对层级。
已退役 Audit/密码学模块专项设计、旧轮次/发布记录及混合历史架构文档不随包安装。
`tests/packaging/check_install.py` 对重定位后的全部安装 Markdown 执行清单与相对文件链接检查；
链接只能命中安装文档树内的真实文件，不能借源码树补齐。该检查不验证外部 URL 或标题锚点。

不要把共享库和静态库覆盖安装到同一前缀。一个前缀描述一个确定的链接变体。

## 下游 CMake / pkg-config

安装包继续提供：

```cmake
find_package(Logger CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

可选组件：

- `shared`
- `static`

CI 使用 `EXACT` + 组件组合检查失败关闭行为；已移除的 `legacy_fork` 与未知组件均必须拒绝。

pkg-config：

```sh
PKG_CONFIG_PATH=<prefix>/lib/pkgconfig pkg-config --cflags --libs logger
```

静态库消费方使用 `pkg-config --static`。

`examples/installed_consumer/` 是发布包的独立下游工程，只能使用已安装的头文件/库；
CI 会检查它的编译命令，禁止回指源码树私有头文件路径。

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

每个 TGZ 同时生成同名 `.sha256`。发布门禁会检查每种类型恰好一个包、校验和文件名
与实际 SHA-256 一致。

XPack 是当前 CI 工具链生成的 原生 Linux/x86_64 候选二进制，不捆绑 glibc，也不代表任意
Linux 发行版、内核或 CPU 自动兼容。

发布验证与正式发布的打包任务固定在 Ubuntu 20.04 / glibc 2.31 / GCC 9 用户态构建，
与 2.0 支持矩阵的最低部署基线一致。GitHub 运行器仍使用 Ubuntu 24.04；构建工具和
完整测试集在 20.04 容器内运行。任意较新宿主上的本地 `xmake pack` 产物不能
据此宣称支持 glibc 2.31，必须另做同等基线构建和解包验收。

最终 TGZ 解包后，共享库 DSO 的 ELF/SONAME/符号门禁强制 `--max-glibc 2.31`。
共享库/静态库两种包都在基线上重新链接并运行 C、C++11、pkg-config消费方与 PIC SDK
模块/加载器，并检查这些最终 ELF 的直接 GLIBC 版本需求不超过 2.31。`liblogger.a` 本身
没有最终动态 ABI；它不捆绑 libc，也不保证在新系统链接的应用能部署到旧 glibc。
部署基线证明还依赖真实基线运行，不能只看归档或只检查数字版本。

随后 Ubuntu 24.04 任务下载并消费同一份基线 TGZ，保留新系统兼容证据，不重新构建 Logger。
新系统链接的消费方允许依赖本机 glibc；包内共享库 DSO 仍受 2.31 上限约束。
正式发布必须同时通过基线打包和新系统消费检查；显式现存 v2 标签授权门禁保持不变。

<a id="release-validation"></a>

## 发布验证

`.github/workflows/release-validation.yml` 对共享库/静态库矩阵分别执行：

1. 完整 Xmake 测试集；
2. 生产产物隔离；
3. 共享库 ABI/SONAME/ELF 类别/机器架构检查；
4. XPack TGZ + SHA-256；
5. 解包后的安装树；
6. 重定位到带空格前缀；
7. C/C++11 CMake 消费方；
8. PIC SDK 模块；
9. pkg-config消费方；
10. 精确版本/组件拒绝；
11. 当前 C/C++ 公开布局/默认值快照；
12. 生产隔离。

测试实现见 `tests/packaging/check_install.py`。该脚本直接读取 `VERSION` 和
`abi/logger-2.0.symbols`，不维护第二份发布版本/ABI 清单。

<a id="crash--断电--sanitizer--基准测试"></a>

## 崩溃 / 断电 / Sanitizer / 基准测试

普通 Logger 的发布验收以基本 API 为范围：参数与返回值、并发、资源、轮转、正常
flush/关闭语义，以及既有 ABI、安装消费、生产隔离和声明的平台兼容性。现有 flush/fsync
契约不变，不增加业务审计、额外丢失保证或物理存储认证要求。

当前 main 保留以下独立证据链，并非每一项都是每次发布的硬门槛
（历史生产版 v1 合同见 release/1.x）：

- `crash-recovery`：8 个普通 Logger 进程 SIGKILL 用例，main/手动共享库/静态库配置各3次；
- `vm-powercut`（可选扩展）：ext4 + XFS 各8个普通 Logger QEMU SIGKILL + 原始磁盘重启/恢复切断点；
  新旧矩阵与三个纯 Audit 检查点切断点的退役原因见 [crash 合同](LOGGER_CRASH_CONTRACT.md)；
- `xmake-parity` Sanitizer：ASan+UBSan 与 TSan 的完整静态库测试集；
- `queue-benchmark`（性能参考）：当前 ABI 2 候选版本用 Xmake，冻结历史基线用其各自提交的原构建定义。

进程 SIGKILL 故障回归和适用的 Sanitizer 检查保留；VM 测试代码与现有自动触发配置不变，
但不要求为每次最终 SHA 单独补跑 VM/掉电矩阵。上述证据仅证明各自明确环境中的实现
行为；基本 API 通过不等于物理存储认证，VM 证据也不能替代最低内核/glibc 兼容性证据。
没有另行约定的业务 SLO 或全面审计不作为本次验收的新增缺陷。

## GitHub Release

`.github/workflows/release-publish.yml` 只接受显式 `v2.*` 标签 push，或指定现存 `v2.x.y`
标签的手动 `workflow_dispatch`。普通 `main` push、`VERSION`、发布说明与工作流
变更均不触发发布。阶段 A 门禁与阶段 B 版本身份迁移的顺序及线上复核要求见
[发布安全](RELEASE_SAFETY.md)。

流程：

1. 从远端获取精确的现存标签，锁定提交 SHA 与标签对象 ID；拒绝缺失标签、分支和非规范 v2 版本；
2. 从锁定提交读取 `VERSION`，验证它与标签、ABI 2、`LOGGER_2.0` 和当前符号清单一致；
3. 要求 `docs/RELEASE_NOTES_<VERSION>.md` 存在，并检查该标签的发布状态；手动触发分支的版本和源码不参与构建；
4. 已有发布只有在标题、prerelease/stable 分类、四个资产与 SHA-256 全部正确时才视为完成并跳过；
5. 发布缺失或不完整时，共享库/静态库分别执行完整 Xmake 发布测试集；
6. 生成并验证 XPack + SHA-256，汇总四个权威发布资产；
7. 构建前和首次发布写操作前重新验证远端标签的提交 SHA 与对象 ID；创建发布
   必须使用 `--verify-tag`，已有发布的修复同样经过来源复核；
8. 发布后重新下载远端四个资产，并再次执行数量、SHA-256、发布类别/标题校验。

`workflow_dispatch` 是已获授权发布的幂等恢复入口：完整发布不会重复发布，不完整
发布从既有标签的源码重建并修复。当前已补齐[2.0.0 发布说明](RELEASE_NOTES_2.0.0.md)，
仍处于待审准备状态；不得为验证门禁创建正式标签、发起发布手动触发或试发布。
正式发布仍需合同确认、精确源码全部适用门禁和单独批准；不能把同一标签改指向另一份源码。

发布证据使用独立的 `SOURCE_COMMIT` 字段记录受测完整 SHA，并记录工作流 run ID、
产物 SHA-256 和现存发布标签的对象 ID/解析提交。仓库中的说明不写入其自身提交 SHA；
发布来源以获批标签解析出的固定提交为准，避免为填入提交号反复产生新提交。

对于历史平台证据，若最终候选的相关源码、公开头文件及构建输入均未变化，并核对
测试入口、配置、工具链与平台条件仍适用，可记录差异核对结果和旧 SHA/run ID，
作为等价承接依据。承接必须标明“未重新执行”，不能把旧结果写成最终 SHA 的新运行；
相关输入变化时应重跑受影响验证。发布包仍须从获批固定源码构建并完成包验证，
`SOURCE_COMMIT` 必须如实记录其实际源码身份，不得用承接证据改写或伪造。

## fork 制品边界

当前 main 不提供旧 fork 辅助接口、兼容头文件或构建选项。所有生产制品检查都必须拒绝
`logger_fork_reinit`、进程创建符号和 `/proc/self/task` 扫描；没有显式启用绕过开关。
CMake/pkg-config元数据不传播旧辅助函数宏，安装检查持续验证公开头文件、当前 ABI 符号集、
共享库/静态库组件和未知/已移除组件的失败关闭行为。PID/atfork 防御和存活对象计数保留。

## 不宣称的兼容性

当前发布工程不证明：

- 真实服务器掉电/重置；
- RAID/HBA/NVMe/SATA 易失写缓存；
- 真实目标 ext4/XFS 存储栈的物理掉电语义；
- v1 明确不支持的网络/分布式文件系统；
- 任意多线程 fork 后复杂库调用安全；
- 32 位；
- 所有内核/glibc 组合；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

平台边界见 `docs/PLATFORM_BASELINE.md`、`docs/V1_RELEASE_CRITERIA.md` 与
`docs/KNOWN_ISSUES.md`。
