# Xmake Build Parity

## 状态

Xmake 当前是 **并行验证构建**，不是发布权威。

- CMake 继续负责完整测试、install、CMake package、pkg-config、CPack 和 GitHub Release。
- Xmake 第一阶段只负责生产 `logger` shared/static artifact。
- Xmake 产物必须通过与 CMake 发布产物相同的 ABI/production-isolation 检查。
- 当前 Xmake 产物不得直接替代正式 release asset。

CI 固定使用 Xmake 3.1.1；工程最低要求提升为 2.8.5：SONAME version support 需要 2.8.2，
内置 `xmake test` / `add_tests` 从 2.8.5 开始提供。

项目版本号现在只从仓库根目录 `VERSION` 读取。CMake、Xmake、生成的
`logger_version.h`、XPack 包名以及 Xmake parity workflow 都消费同一个版本源；
升级版本不再同时修改 `CMakeLists.txt` 与 `xmake.lua`。

## 第一阶段覆盖

Xmake 已表达以下生产契约：

- Linux/ELF only；
- GNU C11；
- shared / static；
- static PIC；
- hidden visibility + public `LOGGER_API`；
- `liblogger.so.0.9.3` / SONAME `liblogger.so.0`；
- `LOGGER_0.9` ELF symbol version；
- public symbol allowlist 直接读取现有 `cmake/logger.symbols`，不维护第二份 ABI 清单；
- `--no-undefined` / `--no-undefined-version`；
- production fault injection disabled；
- builtin SHA-256 only；
- optional legacy fork helper。

Xmake 没有采用内置 `mode.release` rule，因为该 rule 默认 strip。当前配置显式保持项目自己的 Release/ABI 语义，而不是让构建框架默认值定义 ABI。

## 使用

Shared Release：

```sh
xmake f -m release --build_shared=y
xmake -j4 logger
```

Static Release：

```sh
xmake f -m release --build_shared=n
xmake -j4 logger
```

生产产物仍使用已有脚本验证：

```sh
python3 scripts/check_production_artifact.py <artifact>
python3 scripts/check_release_abi.py <shared-elf>
```

## 第二阶段：core test parity

已加入 `build_tests` 可选配置，并使用 Xmake 自带的 `add_tests` / `xmake test` 注册和运行
25 个**直接链接 production logger** 的核心测试。该集合覆盖基础 Logger、rotation、Audit、
Console、format/context/permissions、fork guard、lifecycle、concurrency、API/ABI、
single-writer、redaction、reinit 和 multi-instance。

这一阶段故意不把 white-box support library、`--wrap`、fault injection 或 crash hook
混进 production-linked 测试，以继续保持生产/测试实现边界。

### Test-only support parity

Xmake 现在还提供一个默认关闭的 `build_private_tests` 配置，构建独立的
`logger_test_support` 静态测试库。该 target 与 production `logger` 分离，额外编译
`src/logger_fault.c` 并启用测试宏，只用于以下现有测试：

- 5 个 file/state error-path cases；
- 4 个 process crash/recovery cases；
- multi-instance syslog test。

该 job 使用 `xmake test -j1`：现有 4 个 recovery case 复用同一组 `crash.audit.*` 测试文件名，
因此不能在同一工作目录并发执行；这与 production 并发模型无关。

这验证了测试专用实现与 production artifact 可以在 Xmake 下保持分离。

### Regression-support parity

Xmake 现在还构建独立的 `logger_regression_support`：与 CMake 相同，它使用生产源码，但只在
私有测试 archive 中关闭 FORTIFY，production `logger` 不受影响。第一批迁移不需要自定义
linker interception 的 regression：

- MPSC queue；
- demand-driven metadata；
- global flush；
- stderr SIGPIPE 的 sync/async/preblocked；
- builtin crypto vectors；
- Audit lifecycle/transaction concurrency；
- SHA-256 vectors/boundaries/invalid/thread contract。

这些 case 使用独立的 `regression-tests` job 串行运行，先验证 support archive 与 test runner
语义。需要自定义 linker interception 的 regression 仍留在 CMake，作为后续单独门禁。

### Sanitizer core parity

Xmake 使用 2.8.3+ 的 sanitizer policies，而不是已经进入弃用路径的 sanitizer build modes。
CI 对 25 个 production-linked core tests 分别运行：

- AddressSanitizer + UndefinedBehaviorSanitizer；
- ThreadSanitizer。

这一步验证 Xmake 能把 sanitizer 同时应用到 production static logger 与测试 executable。
完整 CMake suite 仍保留为 sanitizer 发布门禁，直到尚未迁移的 regression 集合完成 parity。

## Crash / VM power-cut parity

Xmake 现在把 focused process-crash 集合补齐为与 CTest 相同的 9 个 case：

- 4 个 Audit checkpoint / fsync crash point；
- 1 个 rotation crash recovery；
- 4 个 file rotation switch crash point。

`xmake-parity` 对 shared/static 两种配置分别执行这 9 个 case，并重复 3 次。测试二进制仍链接
独立的 `logger_test_support`，不会把 crash hook 带入 production `logger`。

QEMU power-cut workflow 也不再只证明 CMake guest：每个既有 cut point 会在同一 runner 上
分别构建 CMake 与 Xmake 的静态 test-only guest，并使用同一个 kernel / 同样的 raw ext4
流程各执行一次。两个构建系统都必须完成同盘重启后的 baseline、恢复、追加和 chain verify。
这仍然是虚拟机存储栈验证，不替代真实硬件断电验收。

## Install parity 3A

Xmake 现在开始验证**安装树外部契约**，仍不承担正式 release packaging。

production `logger` target 直接声明并安装：

- public headers 与生成的 `logger_version.h`；
- shared/static production library；
- 可重定位 `LoggerConfig.cmake` / `LoggerConfigVersion.cmake` /
  `LoggerTargets.cmake`；
- `logger.pc`。

Xmake 安装树继续复用现有 `tests/packaging/check_install.py`，不是建立另一套“Xmake 自测”。
该检查会把 staged prefix 移动到带空格的新位置后，再验证：

- `find_package(Logger 0.9.3 EXACT CONFIG REQUIRED)`；
- shared/static component fail-closed；
- C 与 C++11 installed consumers；
- PIC static library 嵌入 SDK MODULE；
- pkg-config consumer；
- frozen old-header consumer；
- public layout/default 一致性；
- production artifact isolation；
- shared ABI/versioned DSO。

CMake 原有路径仍使用真实 `DESTDIR + cmake --install`。Xmake 3A 使用官方
`xmake install -o <staged-prefix>`，先证明安装树和 relocation 语义；若这一层稳定，
再单独决定是否需要项目级 DESTDIR compatibility wrapper，而不是伪装 Xmake 原生命令已经
提供与 CMake 完全相同的 CLI 语义。

## Package parity 3B

在 install parity 3A 通过后，Xmake 使用内置 XPack 生成 `targz` 二进制包，而不是自己
拼 shell `tar` 命令。包名与当前 release asset 契约保持一致：

`prod-c-logger-0.9.3-Linux-x86_64-{shared|static}.tar.gz`

XPack 完成后由 Xmake `hash.sha256()` 生成同名 `.sha256` 记录。CI 首先复用现有
`scripts/check_release_assets.py`，验证 shared/static 每种包恰好一个、摘要记录文件名正确、
实际 SHA-256 匹配。

随后 CI 解包 TGZ，并再次调用同一 `check_install.py --installed-prefix`，验证包内实际
安装树仍满足 CMake consumer、C++11、SDK MODULE、pkg-config、ExactVersion/components、
旧 header、ABI 与 production isolation。也就是说 package parity 不以“tar 能生成”为通过。

本阶段还把 CMake 当前分发的文档树纳入 Xmake install/XPack：顶层 API/SECURITY/TESTING/
CHANGELOG、非 HISTORY 的 `docs/*.md`、以及 `examples/installed_consumer`。历史 README/API
继续按 CMake 规则排除。

## CPack / XPack structure parity

正式切换 release-publish 之前，CI 从**同一 commit**同时构建 CPack 与 XPack 的 shared/static
TGZ，并解包后比较安装树的相对路径类型与 shared-library symlink target。第一轮采用严格
结构集合一致门禁，不预先把差异列入白名单。

对于来自源码的 public headers、generated version header、文档与 installed-consumer 示例，
还要求逐文件内容一致。library binary 与 CMake/pkg-config metadata 不要求字节一致：
它们继续由现有 ABI、production-isolation、ExactVersion/components 和真实 consumer gate
验证语义，而不是把不同构建系统的实现细节误当作 ABI。

若严格结构集合出现差异，只有明确证明属于构建系统内部 metadata 分片、且外部 consumer
契约已经等价验证的项目才允许后续收窄比较范围；缺失 public/install contract 文件必须修复。

## Wrapped regression parity — Group A

Xmake 现在开始迁移 CMake 中依赖 GNU ld `--wrap` 的 white-box 回归，但仍按风险拆组。

Group A 直接复用现有 `logger_regression_support` 与原测试源码，覆盖：

- queue wait/flush pending/flush watermark；
- I/O accounting 的 drop/fallback/short-write/fsync/rotation/spill/sync；
- lexical resource cleanup；
- Audit commit/checkpoint/lifecycle acquire；
- checkpoint write/fsync/rename/close；
- digest provider failure；
- Audit tail evidence/truncate/active fsync。

Xmake 仅在对应测试 executable 链接阶段增加与 CMake 相同的
`-Wl,--wrap=<symbol>`，不修改 production logger，也不把测试 hook 放入安装或 XPack。

## Wrapped regression parity — Group B

第二组继续复用同一个 `logger_regression_support`，迁移 process fork 与 global facade
生命周期/取消路径：

- `process_fork_regression`：atfork 注册、继承 runtime 拒绝、锁持有窗口、注册窗口、
  earlier handler、prepare/bypass/prefork，以及 global/explicit/Console/context/Audit/verify
  的注册边界；
- `global_lifecycle_regression`：generation admission、bootstrap/start/stop gate、reentry、
  acquire-error、shutdown/fsync 与 child 路径；
- `global_cancel_regression`：write/queue/flush/reopen/reader/create/shutdown/bootstrap/
  control-wait/restore-policy 取消恢复。

这些 executable 使用与 CMake 相同的 `--wrap` 符号集合。CMake 对 process-fork case 的
退出码 77 使用 `SKIP_RETURN_CODE`；Xmake 当前文档没有等价的 skip-return-code 配置，
因此该 target 使用局部 `on_test` runner，把 77 保持为非失败结果，同时仍执行每 case
12 秒超时。CMake 还把每个 CTest case 放进独立的 `test-work/<test>` 工作目录；该 runner
同样为每个 process-fork case 创建独立工作目录，防止 audit lock/log/state 等持久测试产物
跨 case 污染。这个适配只存在于测试 target，不影响 production logger。

## Wrapped regression parity — Group C

第三组迁移 explicit instance 与 Console 的取消/reentry 白盒测试：

- `explicit_scope_regression`：instance write/source/sync/format/queue/fallback/flush/reopen/
  syslog metrics/create/destroy 的 cancellation；
- Console print/info/warn/error/verbose/debug/source cancellation；
- write/worker/Console/create/format/reopen/destroy/global-worker 多入口 reentry；
- restore-policy、async create reject、setup failure、stdio first-error、context alias、
  debug overflow 与 invalid-level contract。

Xmake 使用与 CMake 相同的 12 个 GNU ld `--wrap` 边界，仅链接到
`logger_regression_support`；production `logger`、安装树与 release assets 不含这些 hook。

## Wrapped regression parity — Group D

第四组按当前 CMake 定义迁移 syslog backend/fault/config/compat/fallback 回归：

- `syslog_backend_regression`：bounded nonblocking I/O、reconnect、endpoint、tag、
  cwd、metrics 与 fork-guard；
- `syslog_fault_regression`：socket/connect/send/clock/close 的 syscall fault matrix；
- `syslog_config_regression`：旧 struct 边界与 tail 兼容；
- `logger_v1_consumer`：冻结 v1 header 对当前实现的兼容验证；
- `syslog_fallback_regression`：pressure 下 fallback/queue 行为。

其中 fault/config/v1/fallback 使用与 CMake 相同的 GNU ld `--wrap` 边界。
此前 Group D 曾误指向不存在的 `test_syslog_fault.c` 并登记了不属于当前 CMake
suite 的 lifetime scenarios；CI 首次实际构建该 target 后暴露问题，本组现已按
`CMakeLists.txt` 重新对齐。

## Wrapped regression parity — Group E

第五组补齐 file backend 的 linker-interception 白盒测试，并继续复用
`logger_regression_support`：

- `file_backend_regression`：owner/lock/path hardening、reopen、rotation、
  retention、cwd/directory rename、active replacement、EINTR 等场景；
- `file_platform_regression`：`renameat2(RENAME_NOREPLACE)` 的 real/ENOSYS/
  EINVAL/EOPNOTSUPP 平台分支；
- `file_global_regression`：global teardown 与重复 stop 无副作用；
- `file_legacy_probe`：owner/reopen/collision/cwd/symlink/hardlink/global-repeat
  黑盒兼容探针；
- `file_audit_close_regression`：reserved audit file 创建与 close 错误路径。

wrapper 集合、测试名称与 timeout 与 CMake 保持一致。已有
`file_audit_regression`（busy/cwd 与四个 process-crash 点）继续由
Xmake private/process-crash 门禁覆盖，不重复引入第二套实现。

## Static sanitizer prerequisite — Audit / crypto regression parity

完整 sanitizer profile 不能只重复当前 25 个 core tests；CMake 的 sanitizer job 在
`LOGGER_ENABLE_INSTALL=OFF` 下执行完整 default static CTest suite。因此先把缺失的
实现行为回归逐组迁到 Xmake。

本组直接复用 CMake 现有源码与 case 名称：

- `audit_record_regression`：40 个严格 record codec/parser case；
- `audit_recovery_strict_regression`：27 个 recovery/checkpoint/archive case；
- `audit_reader_regression`：16 个 bounded reader case；
- `crypto_builtin_only_regression`：2 个链接真实 production logger 的 builtin SHA-256
  lifecycle / historical fixture case；
- `sha256_only_regression`：12 个 public/private boundary 与历史状态拒绝 case。

前三者和 `sha256_only_regression` 链接 `logger_regression_support`；
`crypto_builtin_only_regression` 与 CMake 一样链接真实 `logger`，避免把 test-only
实现误当成 production crypto boundary。fixture 参数使用工程绝对路径，不依赖 Xmake
默认运行目录。

## Static sanitizer prerequisite — remaining default-static regressions

Audit/crypto parity 之后，本组补齐 CMake sanitizer default-static suite 中剩余的实现行为
target（shared-only 与 legacy-fork opt-in 目标不属于这个 profile）：

- production isolation 与显式 test-hook isolation，各 13 个 case；
- source ownership 5 个、destroy status 4 个、host format 1 个；
- global stress 3 个、Console host 3 个、benchmark accounting smoke 1 个；
- host-owned integration example 1 个、host ownership regression 10 个。

`test_hooks_regression` 复用 fault-enabled `logger_test_support`；production isolation
继续使用 fault-disabled `logger_regression_support`。Console host 与 host-owned example
继续链接真实 production `logger`。

host-owned case 需要运行时加载 `example_sdk`。Xmake 使用
`add_deps("example_sdk", {inherit = false})` 只建立构建依赖，不把 SDK 链入 host；
测试 runner 从 dependency target 取得实际 shared-library path 并作为参数传入，同时为
每个 case 创建独立工作目录。

## Full sanitizer regression parity

`xmake-parity` 的 sanitizer job 现在保留原有 `sanitizer-core` job id（避免无意义地
更改 branch-protection check 名），但语义已经从 core-only 提升为完整 default-static suite：

- Debug、static logger，与 CMake sanitizer 配置一致；
- address+undefined 与 thread 两个 profile；
- 同时启用 production-linked core、fault-enabled private tests 与全部 regression tests；
- `xmake test -j1` 运行完整测试集合；CTest 使用 `-j2`，但它为每个 case 设置独立
  working directory。Xmake 尚未对所有 case 做全局 working-directory 隔离，因此这里
  先串行执行，测试内部的真实并发/取消/fork/stress 行为不变；
- sanitizer 结束后再对实际 `liblogger.a` 运行
  `scripts/check_production_artifact.py`，对应 CMake sanitizer suite 中仍然存在的
  `crypto_production_artifact_isolation`；
- sanitizer 配置不承担 install/package parity，因为 CMake 在 sanitizer 模式下同样默认
  `LOGGER_ENABLE_INSTALL=OFF`。

## 尚未迁移

以下仍由 CMake 独占，未达到 parity 前不得删除 CMake：

1. CMake 式 DESTDIR CLI parity（Xmake staged install/relocation 已通过 3A）；
2. release-publish 仍只使用 CMake/CPack；XPack 只作为并行 release-asset parity。

## 下一门禁

CPack / XPack 安装树与资产结构、linker-interception regression Group A–E，以及
default-static sanitizer profile 都已进入 CI。当前下一门禁是单独决定是否需要项目级
DESTDIR CLI compatibility；之后才讨论让 release-publish 切换到 Xmake。在正式切换前，
GitHub Release 仍只信任 CMake/CPack。
