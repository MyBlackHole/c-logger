# Xmake 构建一致性

## 状态

Xmake 当前是 **生产发布构建权威**。

- 生产 共享库/静态库、安装元数据、XPack TGZ 与 GitHub 发布资产 由 Xmake 生成；
- `release-validation` 与 `release-publish` 对最终 XPack 包执行 ABI、生产隔离、
  校验和 与真实 已安装消费方契约；
- 发布 CMake 参考门禁 已退役，GitHub 发布 不再依赖 CMake/CTest/CPack 成功；
- 已安装 CMake 消费方 测试继续保留，因为 `find_package(Logger ... CONFIG)` 是发布包的
  对外兼容契约，而不是本项目的构建权威；
- CPack 不再参与正式 发布 或 软件包结构一致性。

CI 固定使用 Xmake 3.1.1；工程最低要求提升为 2.8.5：SONAME 版本支持 需要 2.8.2，
内置 `xmake test` / `add_tests` 从 2.8.5 开始提供。

项目版本号只从仓库根目录 `VERSION` 读取。Xmake 描述域不开放文件 I/O，因此调用方
先执行 `export LOGGER_PROJECT_VERSION="$(cat VERSION)"`，随后 配置/构建/测试/打包
及其子进程都读取同一环境变量。CI 使用 `GITHUB_ENV` 在每个 Xmake 任务 内固定该值。
生成的 `logger_version.h`、XPack 包名以及所有 CI（包括 VM 断电）都不维护第二份
发布版本号。

## 第一阶段覆盖

Xmake 已表达以下生产契约：

- 仅 Linux/ELF；
- GNU C11；
- 共享库 / 静态库；
- 静态库 PIC；
- 隐藏可见性 + 公开 `LOGGER_API`；
- `liblogger.so.<VERSION>` / SONAME `liblogger.so.0`；
- `LOGGER_0.9` ELF symbol version；
- 公开 符号允许列表 直接读取现有 `abi/logger.symbols`，不维护第二份 ABI 清单；
- `--no-undefined` / `--no-undefined-version`；
- 生产 关闭故障注入；
- 仅内置 SHA-256；
- 可选旧版 fork 辅助接口。

Xmake 没有采用内置 `mode.release` rule，因为该 rule 默认 符号剥离。当前配置显式保持项目自己的 发布/ABI 语义，而不是让构建框架默认值定义 ABI。

## 使用

共享库发布构建：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_shared=y
xmake -j4 logger
```

静态库发布构建：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_shared=n
xmake -j4 logger
```

生产产物仍使用已有脚本验证：

```sh
python3 scripts/check_production_artifact.py <artifact>
python3 scripts/check_release_abi.py <shared-elf>
```

## 第二阶段：核心测试一致性

已加入 `build_tests` 可选配置，并使用 Xmake 自带的 `add_tests` / `xmake test` 注册和运行
25 个**直接链接 生产 logger** 的核心测试。该集合覆盖基础 Logger、轮转、Audit、
Console、format/context/permissions、fork guard、生命周期、concurrency、API/ABI、
单写入器、脱敏、重新初始化 和 多实例。

这一阶段故意不把 白盒支持库、`--wrap`、故障注入 或 崩溃钩子
混进 链接生产库 测试，以继续保持生产/测试实现边界。

### 仅测试支持一致性

Xmake 现在还提供一个默认关闭的 `build_private_tests` 配置，构建独立的
`logger_test_support` 静态测试库。该 target 与 生产 `logger` 分离，额外编译
`src/logger_fault.c` 并启用测试宏，只用于以下现有测试：

- 5 个 文件/状态错误路径用例；
- 4 个 进程崩溃/恢复用例；
- 多实例 Syslog 测试。

该 任务 使用 `xmake test -j1`：现有 4 个 恢复用例 复用同一组 `crash.audit.*` 测试文件名，
因此不能在同一工作目录并发执行；这与 生产 并发模型无关。

这验证了测试专用实现与 生产产物 可以在 Xmake 下保持分离。

### 回归支持一致性

Xmake 现在还构建独立的 `logger_regression_support`：与 CMake 相同，它使用生产源码，但只在
私有测试 归档 中关闭 FORTIFY，生产 `logger` 不受影响。第一批迁移不需要自定义
链接器拦截 的 回归：

- MPSC 队列；
- demand-driven 元数据；
- global flush；
- stderr SIGPIPE 的 sync/async/preblocked；
- builtin crypto vectors；
- Audit 生命周期/transaction concurrency；
- SHA-256 vectors/boundaries/invalid/thread 契约。

这些 用例 使用独立的 `regression-tests` 任务 串行运行，先验证 支持归档 与 测试运行器
语义。需要自定义 链接器拦截 的 回归 仍留在 CMake，作为后续单独门禁。

### Sanitizer 核心一致性

Xmake 使用 2.8.3+ 的 Sanitizer 策略，而不是已经进入弃用路径的 sanitizer 构建模式。
CI 对 25 个 链接生产库的核心测试 分别运行：

- AddressSanitizer + UndefinedBehaviorSanitizer；
- ThreadSanitizer。

这一步最初验证 Xmake 能把 sanitizer 同时应用到 生产 static logger 与测试 executable。
后续完整 回归 parity 已迁入 Xmake，旧 CMake sanitizer gate 也已随 native CI 退役。

## Crash / VM 断电 parity

Xmake 现在把 专项进程崩溃 集合固定为 9 个 用例：

- 4 个 Audit checkpoint / fsync 崩溃点；
- 1 个 轮转 crash 恢复；
- 4 个 file 轮转 switch 崩溃点。

这 9 个 `add_tests` 统一使用 Xmake 原生 `group = "process-crash"`。
专用 `crash-recovery` 工作流 对 共享库/静态库 两种配置分别执行该 分组 3 次，
`scripts/check_crash_matrix.py` 从实际 Xmake 结果日志 核对**精确 用例 集合**，任何缺失、
意外新增或失败都会使门禁失败，同时保留 JSON、JUnit 和原始日志证据。

此前 `xmake-parity` 内重复维护的一份 9×3 用例 列表已经删除，避免同一 crash 测试集 双跑。
测试二进制仍链接独立的 `logger_test_support`，不会把 崩溃钩子 带入 生产 `logger`。

QEMU 断电 工作流 已切到 Xmake-only authority。迁移前 CI 已对同一 10 个 cut point
同时运行 CMake/Xmake 来宾系统，确认两者都能完成同盘重启后的 基线、恢复、追加与 链
verify；当前不再为每个 cut point 重复执行旧 CMake 来宾系统。

权威 来宾系统 仍是静态 仅测试 `vm_powercut_guest`，链接 `logger_test_support` 并保留
`--wrap=logger_fault_crash_if_requested`。每个 cut point 继续执行真实 QEMU SIGKILL、raw ext4
重启恢复和 链验证；这仍然是虚拟机存储栈验证，不替代真实硬件断电验收。

## 安装一致性 3A

Xmake 现在开始验证**安装树外部契约**，仍不承担正式 发布 packaging。

生产 `logger` target 直接声明并安装：

- 公开头文件 与生成的 `logger_version.h`；
- 共享库/静态库 生产 library；
- 可重定位 `LoggerConfig.cmake` / `LoggerConfigVersion.cmake` /
  `LoggerTargets.cmake`；
- `logger.pc`。

Xmake 安装树继续复用现有 `tests/packaging/check_install.py`，不是建立另一套“Xmake 自测”。
该检查会把 暂存前缀 移动到带空格的新位置后，再验证：

- `find_package(Logger 0.9.3 EXACT CONFIG REQUIRED)`；
- 共享库/静态库 component fail-closed；
- C 与 C++11 installed consumers；
- PIC static library 嵌入 SDK MODULE；
- pkg-config 消费方；
- 冻结旧头文件消费方；
- 公开 layout/default 一致性；
- 生产产物隔离；
- 共享 ABI/版本化 DSO。

CMake 原有路径仍使用真实 `DESTDIR + cmake --install`。Xmake 3A 使用官方
`xmake install -o <staged-prefix>`，先证明安装树和 重定位 语义；若这一层稳定，
再单独决定是否需要项目级 DESTDIR 兼容包装器，而不是伪装 Xmake 原生命令已经
提供与 CMake 完全相同的 CLI 语义。

## 软件包一致性 3B

在 install parity 3A 通过后，Xmake 使用内置 XPack 生成 `targz` 二进制包，而不是自己
拼 shell `tar` 命令。包名与当前 发布 asset 契约保持一致：

`prod-c-logger-0.9.3-Linux-x86_64-{shared|static}.tar.gz`

XPack 完成后由 Xmake `hash.sha256()` 生成同名 `.sha256` 记录。CI 首先复用现有
`scripts/check_release_assets.py`，验证 共享库/静态库 每种包恰好一个、摘要记录文件名正确、
实际 SHA-256 匹配。

随后 CI 解包 TGZ，并再次调用同一 `check_install.py --installed-prefix`，验证包内实际
安装树仍满足 CMake consumer、C++11、SDK MODULE、pkg-config、ExactVersion/组件、
旧 header、ABI 与 生产 isolation。也就是说 软件包一致性 不以“tar 能生成”为通过。

本阶段还把 CMake 当前分发的文档树纳入 Xmake install/XPack：顶层 API/SECURITY/TESTING/
CHANGELOG、非 HISTORY 的 `docs/*.md`、以及 `examples/installed_consumer`。历史 README/API
继续按 CMake 规则排除。

## CPack / XPack 结构一致性 — completed

在 发布 authority 切换前，CI 已从同一 commit 对 共享库/静态库 的 CPack 与 XPack TGZ
执行严格安装树、symlink 与 source-derived file 对照；该门禁已完成迁移验证使命并退役。

当前不再为每个提交重复生成 CPack。XPack 包继续通过 ABI、生产隔离、
ExactVersion/组件、真实 C/C++ consumer、SDK MODULE、pkg-config、旧 header 与
安装树 重定位 检查来验证对外契约；这些检查直接针对最终发布包，而不是依赖旧构建
系统作为 判定基准。

## 链接器包装回归一致性 — A 组

Xmake 现在开始迁移 CMake 中依赖 GNU ld `--wrap` 的 white-box 回归，但仍按风险拆组。

A 组 直接复用现有 `logger_regression_support` 与原测试源码，覆盖：

- 队列等待/刷新等待/刷新水位；
- I/O 记账 的 丢弃/回退/短写/fsync/轮转/溢出区/sync；
- 词法作用域资源清理；
- Audit 提交/checkpoint/生命周期 acquire；
- checkpoint write/fsync/rename/close；
- digest 提供方 failure；
- Audit 尾部证据/truncate/活动文件 fsync。

Xmake 仅在对应测试 executable 链接阶段增加与 CMake 相同的
`-Wl,--wrap=<symbol>`，不修改 生产 logger，也不把测试 hook 放入安装或 XPack。

## 链接器包装回归一致性 — B 组

第二组继续复用同一个 `logger_regression_support`，迁移 process fork 与 全局门面
生命周期/取消路径：

- `process_fork_regression`：atfork 注册、继承 运行时 拒绝、锁持有窗口、注册窗口、
  earlier handler、prepare/bypass/prefork，以及 global/explicit/Console/context/Audit/verify
  的注册边界；
- `global_lifecycle_regression`：代次准入、引导/启动/停止门禁、重入、
  acquire-error、shutdown/fsync 与 child 路径；
- `global_cancel_regression`：write/队列/flush/reopen/reader/create/shutdown/bootstrap/
  control-wait/恢复策略 取消恢复。

这些 executable 使用与 CMake 相同的 `--wrap` 符号集合。CMake 对 process-fork 用例 的
退出码 77 使用 `SKIP_RETURN_CODE`；Xmake 当前文档没有等价的 skip-return-code 配置，
因此该 target 使用局部 `on_test` 运行器，把 77 保持为非失败结果，同时仍执行每 用例
12 秒超时。CMake 还把每个 CTest 用例 放进独立的 `test-work/<test>` 工作目录；该 运行器
同样为每个 process-fork 用例 创建独立工作目录，防止 audit lock/log/state 等持久测试产物
跨 用例 污染。这个适配只存在于测试 target，不影响 生产 logger。

## 链接器包装回归一致性 — C 组

第三组迁移 显式实例 与 Console 的取消/重入 白盒测试：

- `explicit_scope_regression`：instance write/source/sync/format/队列/fallback/flush/reopen/
  syslog 指标/create/destroy 的 取消；
- Console print/info/warn/error/verbose/debug/source 取消；
- write/worker/Console/create/format/reopen/destroy/global-worker 多入口 重入；
- 恢复策略、async create reject、setup failure、stdio 首错、上下文别名、
  debug overflow 与 非法级别契约。

Xmake 使用与 CMake 相同的 12 个 GNU ld `--wrap` 边界，仅链接到
`logger_regression_support`；生产 `logger`、安装树与 发布 assets 不含这些 hook。

## 链接器包装回归一致性 — D 组

第四组按当前 CMake 定义迁移 syslog 后端/故障/配置/兼容/回退 回归：

- `syslog_backend_regression`：bounded nonb锁机制 I/O、reconnect、endpoint、tag、
  cwd、指标 与 fork-guard；
- `syslog_fault_regression`：socket/connect/send/clock/close 的 syscall fault matrix；
- `syslog_config_regression`：旧 struct 边界与 tail 兼容；
- `logger_v1_consumer`：冻结 v1 header 对当前实现的兼容验证；
- `syslog_fallback_regression`：压力 下 fallback/队列 行为。

其中 fault/config/v1/fallback 使用与 CMake 相同的 GNU ld `--wrap` 边界。
此前 D 组 曾误指向不存在的 `test_syslog_fault.c` 并登记了不属于当前 CMake
测试集 的 生命周期场景；CI 首次实际构建该 target 后暴露问题，本组现已按
`CMakeLists.txt` 重新对齐。

## 链接器包装回归一致性 — E 组

第五组补齐 file backend 的 链接器拦截 白盒测试，并继续复用
`logger_regression_support`：

- `file_backend_regression`：所有者/锁/路径加固、reopen、轮转、
  保留策略、cwd/目录重命名、活动文件替换、EINTR 等场景；
- `file_platform_regression`：`renameat2(RENAME_NOREPLACE)` 的 real/ENOSYS/
  EINVAL/EOPNOTSUPP 平台分支；
- `file_global_regression`：全局拆除 与重复 stop 无副作用；
- `file_legacy_probe`：owner/reopen/collision/cwd/symlink/hardlink/global-repeat
  黑盒兼容探针；
- `file_audit_close_regression`：reserved audit file 创建与 close 错误路径。

包装器 集合、测试名称与 超时 与 CMake 保持一致。已有
`file_audit_regression`（busy/cwd 与四个 process-crash 点）继续由
Xmake private/process-crash 门禁覆盖，不重复引入第二套实现。

## 静态 Sanitizer 前置条件 — Audit / crypto 回归 parity

完整 sanitizer 配置 不能只重复当前 25 个 核心测试；CMake 的 sanitizer 任务 在
`LOGGER_ENABLE_INSTALL=OFF` 下执行完整 默认静态 CTest 测试集。因此先把缺失的
实现行为回归逐组迁到 Xmake。

本组直接复用 CMake 现有源码与 用例 名称：

- `audit_record_regression`：40 个严格 record codec/解析器 用例；
- `audit_recovery_strict_regression`：27 个 恢复/检查点/归档 用例；
- `audit_reader_regression`：16 个 有界读取器 用例；
- `crypto_builtin_only_regression`：2 个链接真实 生产 logger 的 builtin SHA-256
  生命周期 / historical 固定数据 用例；
- `sha256_only_regression`：12 个 公开/private boundary 与历史状态拒绝 用例。

前三者和 `sha256_only_regression` 链接 `logger_regression_support`；
`crypto_builtin_only_regression` 与 CMake 一样链接真实 `logger`，避免把 仅测试
实现误当成 生产密码边界。固定数据 参数使用工程绝对路径，不依赖 Xmake
默认运行目录。

## 静态 Sanitizer 前置条件 — 剩余默认静态库回归

Audit/crypto parity 之后，本组补齐 CMake sanitizer default-static 测试集 中剩余的实现行为
target（shared-only 与 旧版-fork 显式启用 目标不属于这个 配置）：

- 生产 isolation 与显式 test-hook isolation，各 13 个 用例；
- source 所有权 5 个、destroy status 4 个、宿主格式化 1 个；
- 全局压力 3 个、Console host 3 个、基准记账冒烟测试 1 个；
- 宿主持有集成示例 1 个、宿主所有权回归 10 个。

`test_hooks_regression` 复用 fault-enabled `logger_test_support`；生产 isolation
继续使用 fault-disabled `logger_regression_support`。Console host 与 host-owned example
继续链接真实 生产 `logger`。

host-owned 用例 需要运行时加载 `example_sdk`。Xmake 使用
`add_deps("example_sdk", {inherit = false})` 只建立构建依赖，不把 SDK 链入 host；
测试 运行器 从 dependency target 取得实际 共享库路径 并作为参数传入，同时为
每个 用例 创建独立工作目录。

## 完整 Sanitizer 回归一致性

`xmake-parity` 的 sanitizer 任务 现在保留原有 `sanitizer-core` 任务 id（避免无意义地
更改 分支保护检查 名），但语义已经从 core-only 提升为完整 default-static 测试集：

- Debug、static logger，与 CMake sanitizer 配置一致；
- address+undefined 与 thread 两个 配置；
- 同时启用 链接生产库 core、fault-enabled 私有测试 与全部 回归 tests；
- `xmake test -j1` 运行完整测试集合；CTest 使用 `-j2`，但它为每个 用例 设置独立
  工作目录。Xmake 尚未对所有 用例 做全局 working-directory 隔离，因此这里
  先串行执行，测试内部的真实并发/取消/fork/stress 行为不变；
- sanitizer 结束后再对实际 `liblogger.a` 运行
  `scripts/check_production_artifact.py`，对应 CMake sanitizer 测试集 中仍然存在的
  `crypto_production_artifact_isolation`；
- sanitizer 配置不承担 install/软件包一致性，因为 CMake 在 sanitizer 模式下同样默认
  `LOGGER_ENABLE_INSTALL=OFF`。

## DESTDIR 策略

CMake 风格的 `DESTDIR=...` CLI 兼容明确设为 **非目标**，不再阻塞发布迁移。
Xmake 使用原生 `xmake install -o <staged-prefix>`，并且 staged install、重定位、
CMake package consumer、pkg-config、ABI/isolation 与最终 XPack 解包后的 installed-consumer
契约 已有独立 CI 门禁。如果后续真实下游明确依赖 CMake 风格 DESTDIR，再单独增加
兼容包装器，而不是把它伪装成 Xmake 原生语义。

## 仅共享库回归一致性

Xmake shared 回归 配置 现在也覆盖 CMake 原先独占的 4 个真实 DSO 测试：

- `syslog_shared_regression`：27 个 syslog backend 场景直接链接真实 shared logger；
- `global_shared_regression`：3 个 全局压力 场景，启用
  `LOGGER_PUBLIC_ABI_TEST`，只检查 公开/shared 可观察状态；
- `logger_v1_shared_consumer`：冻结 v1 header 直接链接当前 shared logger；
- `liblogger_dlclose_test`：测试 executable 不链接 logger，只以 仅顺序依赖
  构建 DSO，并将实际 `liblogger.so.<VERSION>` 路径传给 `dlopen`。

这些 target 只在 `--build_shared=y --build_regression_tests=y` 时注册，因此不会污染
static 配置。

## 旧版 fork 辅助接口一致性

最后一个 CMake-only 回归目标也已迁入 Xmake：

- `logger_regression_support` / `logger_test_support` 在 `--legacy_fork=y` 时与
  CMake 的 `LOGGER_SOURCES` 一致，额外编译 `src/logger_fork.c` 并导出
  `LOGGER_ENABLE_LEGACY_FORK_HELPER=1`；
- `fork_reinit_regression` 使用与 CMake 相同的 5 个 GNU ld 包装器，覆盖 23 个
  受控单线程 fork/继续执行 场景；
- `fork_reinit_example` 也由 Xmake 构建；
- 默认 发布 仍保持 旧版 helper 关闭；独立 `legacy-fork` CI 任务 显式以
  `--legacy_fork=y` 构建 example 并逐个执行 23 个回归 用例。

## 剩余工具/示例一致性

CMake 剩余的 4 个非测试 executable/tool/example 也已迁入 Xmake：

- `bench_logger`：链接 生产 logger；
- `multi_instance_example`：链接 生产 logger；
- `syslog_client_example`：链接 生产 logger，并保留 CMake 的严格编译告警；
- `crypto_chain_tool`：继续链接 `logger_regression_support`，供
  `scripts/crypto_cross_version.py` 使用。

另外 `fork_reinit_example` 已从 回归-only scope 移到真正的
`--legacy_fork=y` 构建表面，不再要求同时启用 `build_regression_tests`。

## 原生 CI 权威

旧 `.github/workflows/ci.yml` 的 shared 发布/ABI、Debug 和两组 sanitizer 门禁已由
Xmake 工作流 覆盖，因此不再保留第二套 CMake CI。为避免删掉全量 工作流 后产生触发
空窗，`xmake-parity` 的 push/pull_request 路径过滤 同时纳入 `tests/**` 与
`examples/**`；测试源码和示例变化会继续触发主 Xmake 门禁。

## 队列基准测试权威

`.github/workflows/queue-benchmark.yml` 的**当前 候选版本** 已改由 Xmake 构建
`bench_matrix`，仍使用同一 `logger_regression_support`、同一 benchmark source 和原比较
脚本/阈值。两个冻结 基线 ref 继续使用各自历史 commit 内的 CMake 构建，这是有意保留
的历史重现方式：删除当前源码树的 CMake 不应改写过去基准二进制的构建定义。

## 迁移完成

项目自身的 CMake build system 已完成退役：

- 生产 共享库/静态库、tests、sanitizer、crash、VM 断电、benchmark 候选版本、
  install 与 packaging 均由 Xmake 驱动；
- ABI allowlist 已迁到中立的 `abi/logger.symbols`；
- 根 `CMakeLists.txt` 与旧 CMake packaging/helper 文件不再属于当前工程；
- `scripts/check.sh` 已切到 Xmake，本地旧 配置 名仅作为保守完整-测试集 兼容别名。

仍然保留的 CMake 内容只有**下游消费兼容面**：`examples/installed_consumer/CMakeLists.txt`
以及 Xmake 在安装树中生成的 `LoggerConfig.cmake` / `LoggerTargets*.cmake`。它们用于验证
`find_package(Logger ... CONFIG)`，不重新引入项目级 CMake 构建权威。

## 发布权威

`release-validation` 以 Xmake/XPack 软件包矩阵 为正式产物门禁；
`release-publish` 只收集通过验证的 XPack 共享库/静态库 TGZ 与 SHA-256 并创建 GitHub
发布。发布、测试和故障恢复证据链不再依赖当前源码树中的 CMake/CTest/CPack。
