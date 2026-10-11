# Testing

> main 的源码、生产库和未安装支持库均为 Audit-free ABI 2。普通 Logger 的生产链接、故障、锁、
> 格式、文件 crash、安装消费与制品隔离独立验收。旧 v1 snapshot 不再是当前门禁。

Xmake 是项目唯一的构建与测试权威。所有测试都由 `xmake.lua` 注册，CI 与本地开发使用同一
目标/测试 定义。下游安装包仍使用真实 CMake 消费方 验证 `find_package(Logger ...)`，
但项目自身不再依赖 CTest。

## 基本约束

运行 Xmake 前先导出唯一版本源：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

默认测试构建使用 Linux/ELF、GNU C11、`-Wall -Wextra -Wpedantic -Werror`。production、
fault/crash 和 白盒回归 分离：

- `logger`：真实 生产产物，故障注入 关闭；
- `logger_test_support`：仅测试使用，包含 `src/logger_fault.c`；
- `logger_regression_support`：同源 white-box archive，用于 GNU ld `--wrap`
  与内部 确定性回归。

后两者永不安装，也不进入 XPack。

## 本地入口

### Fast

仅运行直接链接 production `logger` 的 核心测试：

```sh
scripts/check.sh fast
```

等价于：

```sh
xmake f -m release -o build \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=n \
  --build_regression_tests=n
xmake test -j4
```

### 生产构建

运行当前 静态库/完整测试面，包括 core、private 故障/崩溃支持 与全部 regression：

```sh
scripts/check.sh production
```

完整 suite 默认串行执行。部分历史测试共享固定文件名或工作目录，而测试内部的真实并发、
取消、fork 和 压力测试 行为不受串行 运行器 影响。

### 专项崩溃测试

8 个普通 Logger 持久性进程崩溃用例使用 Xmake 原生 `process-crash` 分组：

```sh
scripts/check.sh crash
# 或
xmake test -g process-crash -j1
xmake test -g crash-verifier -j1
```

固定集合：已确认 baseline、普通 data fsync 前/后、真实轮转后的 data fsync 后、以及
四个现有 file rotation switch 切断点。每项都在 pipe 确认真实切点后由父进程 SIGKILL，
枚举 active + archives 逐字节验证唯一 baseline，再重开、追加和验证两个 recovery 记录。
未确认 pre-fsync 尾部与已确认记录分开验收，普通 Logger 不自动修复 torn tail。

原 9 项中的三个纯 Audit checkpoint 点明确退役；普通 Logger 没有对应 checkpoint 合同。
详细新旧映射、未确认尾部规则和真实切点见 [普通 Logger crash 合同](docs/LOGGER_CRASH_CONTRACT.md)。
专用 `crash-recovery` CI 在 main/manual 对 shared/static 配置各执行 3 次，PR执行shared配置一次；
私有 crash binary 始终链接静态测试支持 archive，不代表生产 shared DSO 注入。
`scripts/check_crash_matrix.py` 核对精确八项并输出 JSON、JUnit 和原始日志。
`crash-verifier` 用正负向 mutation fixture 验证验收器不会放过缺失、重复或损坏。

### Raw fork 防御

生产链接的 `fork_test` / `fork_guard_test` 与白盒 `process_fork_regression` 持续验证
PID/atfork 防护、注册窗口、早注册 child handler、继承锁和 ECHILD 快速拒绝。
`process_fork_regression` 在 shared 配置下仍链接同源 static 支持库；production core
测试分别覆盖真实 shared/static 产物。旧 `legacy-fork` profile 与重初始化 helper 已移除。

## 旧 profile 参数

历史 `scripts/check.sh unit/integration/concurrency/reliability/security/host-owned`
依赖 CTest 多标签。Xmake 的 test 分组 是单组语义，无法无损表达同一 case 同时属于多个标签。

为避免旧命令变成“看起来成功但实际少跑”的兼容陷阱，这些参数当前仍接受，但会运行**完整
Xmake suite 的保守超集**。后续只有在真实开发工作流需要时，才新增 Xmake 原生 focused 分组；
不会为了复刻旧标签体系重新维护第二份测试分类表。

## Sanitizer 检测

CI 对完整 default-static suite 运行两组 Sanitizer：

ASan + UBSan：

```sh
xmake f -m debug -o build-asan \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y \
  --policies=build.sanitizer.address,build.sanitizer.undefined
xmake test -j1
```

TSan：

```sh
xmake f -m debug -o build-tsan \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y \
  --policies=build.sanitizer.thread
xmake test -j1
```

Sanitizer 后仍对生成的 production `liblogger.a` 运行
`scripts/check_production_artifact.py`，确保 test hook 没有进入生产产物。

## 共享库集成

共享库配置 除 生产库链接 核心测试 外还覆盖真实 DSO 边界，包括：

- 共享库 Syslog 后端；
- global shared 压力测试；
- 当前 C/C++11 公开 ABI 消费方；
- `dlopen/dlclose` 生命周期；
- installed package 消费方。

需要完整 shared suite：

```sh
xmake f -m release -o build-shared-tests \
  --build_shared=y \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y
xmake test -j1
```

white-box `--wrap` regression 即使在 共享库配置 中也链接 同源 static
`logger_regression_support`；只有需要验证真实 DSO 观察面的 case 才直接链接 production shared
library。不能把 static wrapper test 误称为 DSO 内部拦截。

## QEMU 断电

`.github/workflows/vm-powercut.yml` 构建 static test-only `vm_powercut_guest`，对相同 8 个普通
Logger cut point 在 ext4 与 XFS raw filesystem image 上执行真实 QEMU SIGKILL、同盘重启、
完整 baseline 字节校验、重新打开和追加验证。PR和main/manual均运行完整8×2矩阵。

- acknowledged baseline；
- `before_file_fsync` / `after_file_fsync` / `rotation_after_file_fsync`；
- `file_after_archive_rename` / `file_after_archive_dirsync`；
- `file_after_active_open` / `file_after_active_dirsync`。

原 10 点中的三个纯 Audit checkpoint 点退役，新增 rotated post-fsync 点。不存在 Audit chain
verify 承诺。真实 syscall/hook、逐项映射与JSON证据见 [crash 合同](docs/LOGGER_CRASH_CONTRACT.md)。

这证明指定运行器上 virtual x86_64 + raw ext4/XFS + QEMU 存储路径的恢复行为，不替代
真实服务器物理断电、RAID/HBA/NVMe/SATA 易失缓存、实际目标挂载/存储栈或最低支持内核验收。
普通进程 SIGKILL 保留宿主页缓存，与终止整台 QEMU 的边界不同。

## Queue 基准测试

当前 candidate 的 `bench_matrix` 由 Xmake 构建。两个冻结历史 baseline commit 继续用各自
commit 中原有的 CMake 构建，以保证历史基准可重现，而不是用今天的构建描述重解释过去数据。

`queue-benchmark` CI 保持原线程数、record size、重复次数、memory reduction threshold 与比较
脚本不变。

## 安装与发布包验证

发布验证 对 共享库/静态库 两种 XPack 都执行：

1. 完整 Xmake suite；
2. 生产产物 isolation；
3. shared ELF ABI / SONAME / 符号版本 检查；
4. XPack TGZ + SHA-256；
5. 解包后的 relocation；
6. 独立 C/C++11 CMake 消费方；
7. PIC SDK 模块；
8. pkg-config 消费方；
9. 精确版本/组件s 失败关闭；
10. 当前 public layout/default/function type 和 config 负向合同。

`examples/installed_consumer/CMakeLists.txt` 和发布包中的 `LoggerConfig.cmake` 是**消费兼容性**
测试资产，不代表项目重新依赖 CMake 构建。

## 工程 gate

测试通过不是唯一准入条件。较大的 queue、工作线程、backend、lifecycle、所有者ship、locking 或
## Observability regression

统一 diagnostics 通过现有真实故障场景验证，而不是单独伪造计数：

- queue full：检查 current depth/capacity、完成积压 与 saturation；
- DROP / SYNC fallback：检查独立 lifetime observation flag；
- long-message spill exhaustion：检查 in-use/capacity/exhaustion；
- backend write/fsync failure：检查 failed_records 与 sticky first_error；
- File sink：检查 write/writev 系统调用、部分字节、data/dir fsync、轮转、重新打开 分类计数；
- flush 后：queue depth、spill in-use、完成积压 回到 0；
- global generation：diagnostics 与 metrics 一样受 pin/stale-generation gate 保护；
- explicit reentry：同线程嵌套读取被 EDEADLK 拒绝并清零输出；
- installed 消费方：使用安装后的 公开头文件/library 实际调用 diagnostics。

public API 修改还必须遵守：

- `docs/ARCHITECTURE_INVARIANTS.md`
- `docs/RESOURCE_OWNERSHIP.md`
- `docs/REFCOUNTING.md`
- `docs/RESOURCE_CLEANUP.md`
- `docs/LOCKING.md`
- `docs/LOCK_MATRIX.md`
- `docs/CONCURRENCY.md`
- `docs/ERROR_HANDLING.md`
- `docs/LIFECYCLE.md`

新增资源获取/转移路径应有对应失败覆盖；passing test 不能替代 所有者ship、锁顺序和最终释放
关系的可证明性。

## Cancellation 边界

调用方处于 `PTHREAD_CANCEL_ASYNCHRONOUS + PTHREAD_CANCEL_ENABLE` 时进入 Logger/Console
不在支持契约内。支持边界是 延迟取消，或调用前已禁用 cancellation。
restore-policy regression 会验证原先 disabled 的 调用方 仍保持 disabled 且 cancellation type
不被破坏；构造函数 契约 会拒绝 enabled asynchronous cancellation。


## 普通 Logger 格式与隔离

静态 production 目标 `logger_format_production_test` 保留 normal、cache-hit、localtime、date、
zone、bad-date、bad-zone、cached-bad、truncation 九项；`format_checked_production_test`
保留容量边界、严格/普通格式一致、printf 失败与非法纳秒断言。独立入口
`tests/regression/run_logger_format.sh BUILD_DIR [address,undefined]` 同样只链接真实生产 archive。

`production_isolation_regression` 直接链接当前 static/shared 生产库，
`test_hooks_regression` 链接未安装 fault-support archive。两者对照 file_write/file_fsync/
file_rename/short-write/syslog-path 与四个真实轮转环境 hook。process_fork 保留17个普通
Logger/Console/context场景；原21项的4个Audit入口专属场景退役。锁失败保留6个普通场景。
逐项迁移记录见 [Audit 清理覆盖映射](validation/AUDIT_TEST_RETIREMENT.md)。
