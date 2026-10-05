# Testing

Xmake 是项目唯一的构建与测试权威。所有测试都由 `xmake.lua` 注册，CI 与本地开发使用同一
目标/测试 定义。下游安装包仍使用真实 CMake 消费方 验证 `find_package(Logger ...)`，
但项目自身不再依赖 CTest。

## 基本约束

运行 Xmake 前先导出唯一版本源：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

默认测试构建使用 Linux/ELF、GNU C11、`-Wall -Wextra -Wpedantic -Werror`。生产、
故障/崩溃 和 白盒回归 分离：

- `logger`：真实 生产产物，故障注入 关闭；
- `logger_test_support`：仅测试使用，包含 `src/logger_fault.c`；
- `logger_regression_support`：同源 白盒归档，用于 GNU ld `--wrap`
  与内部 确定性回归。

后两者永不安装，也不进入 XPack。

## 本地入口

### Fast

仅运行直接链接 生产 `logger` 的 核心测试：

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

运行当前 静态库/完整测试面，包括 core、private 故障/崩溃支持 与全部 回归：

```sh
scripts/check.sh production
```

完整 suite 默认串行执行。部分历史测试共享固定文件名或工作目录，而测试内部的真实并发、
取消、fork 和 压力测试 行为不受串行 运行器 影响。

### 专项崩溃测试

9 个 持久性 进程崩溃用例 使用 Xmake 原生 `process-crash` 分组：

```sh
scripts/check.sh crash
# 或
xmake test -g process-crash -j1
```

集合固定为：

- 4 个 Audit 检查点/fsync 切断点；
- 1 个 Audit 轮转 崩溃恢复；
- 4 个 file 轮转 switch 切断点。

专用 `crash-recovery` CI 会 共享库/静态库 各执行 3 次，并用
`scripts/check_crash_matrix.py` 从实际 Xmake log 核对精确集合，输出 JSON、JUnit 和原始日志。

### 旧版 fork 兼容性

默认 发布 不编译 旧版辅助接口。需要受控兼容测试时：

```sh
scripts/check.sh legacy-fork
```

或显式：

```sh
xmake f -m release -o build-legacy \
  --build_shared=n \
  --legacy_fork=y \
  --build_regression_tests=y
xmake test 'fork_reinit_regression/*' -j1
```

## 旧 profile 参数

历史 `scripts/check.sh unit/integration/concurrency/reliability/security/host-owned/crypto`
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

Sanitizer 后仍对生成的 生产 `liblogger.a` 运行
`scripts/check_production_artifact.py`，确保 test hook 没有进入生产产物。

## 共享库集成

共享库配置 除 生产库链接 核心测试 外还覆盖真实 DSO 边界，包括：

- 共享库 Syslog 后端；
- global shared 压力测试；
- frozen v1 公开头文件 消费方；
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

white-box `--wrap` 回归 即使在 共享库配置 中也链接 同源 static
`logger_regression_support`；只有需要验证真实 DSO 观察面的 case 才直接链接 生产 shared
library。不能把 static 包装器 test 误称为 DSO 内部拦截。

## QEMU 断电

`.github/workflows/vm-powercut.yml` 构建 static test-only `vm_powercut_guest`，对 10 个 cut
point 在 ext4 与 XFS 两种 raw 文件系统 image 上执行真实 QEMU SIGKILL、同盘重启、恢复、
追加与 Audit chain verify。

覆盖：

- acknowledged baseline；
- `before_audit_fsync` / `after_audit_fsync`；
- `before_state_rename` / `after_state_rename` / `after_checkpoint_commit`；
- `file_after_archive_rename` / `file_after_archive_dirsync`；
- `file_after_active_open` / `file_after_active_dirsync`。

这证明 GitHub 运行器 上 virtual x86_64 + raw ext4/XFS + QEMU 存储路径的恢复行为，不替代
真实服务器断电、RAID/HBA/NVMe/SATA 易失缓存、实际目标 挂载/存储栈 或最低
支持 内核 的验收。

## Queue 基准测试

当前 candidate 的 `bench_matrix` 由 Xmake 构建。两个冻结历史 baseline commit 继续用各自
commit 中原有的 CMake 构建，以保证历史基准可重现，而不是用今天的构建描述重解释过去数据。

`queue-benchmark` CI 保持原线程数、record size、重复次数、memory reduction threshold 与比较
脚本不变。

## 安装与发布包验证

发布验证 对 共享库/静态库 两种 XPack 都执行：

1. 完整 Xmake suite；
2. 生产产物 隔离；
3. 共享 ELF ABI / SONAME / 符号版本 检查；
4. XPack TGZ + SHA-256；
5. 解包后的 relocation；
6. 独立 C/C++11 CMake 消费方；
7. PIC SDK 模块；
8. pkg-配置 消费方；
9. 精确版本/组件s 失败关闭；
10. frozen 旧头文件 与 public layout/default 兼容。

`examples/installed_consumer/CMakeLists.txt` 和发布包中的 `LoggerConfig.cmake` 是**消费兼容性**
测试资产，不代表项目重新依赖 CMake 构建。

## 工程 gate

测试通过不是唯一准入条件。较大的 queue、工作线程、backend、lifecycle、所有者ship、locking 或
## Observability 回归

统一 诊断 通过现有真实故障场景验证，而不是单独伪造计数：

- queue full：检查 current depth/capacity、完成积压 与 saturation；
- DROP / SYNC 回退：检查独立 lifetime observation flag；
- long-message spill exhaustion：检查 in-use/capacity/exhaustion；
- backend write/fsync failure：检查 failed_records 与 sticky first_error；
- File sink：检查 write/writev 系统调用、部分字节、数据/目录 fsync、轮转、重新打开 分类计数；
- 刷新 后：queue depth、spill in-use、完成积压 回到 0；
- global generation：诊断 与 metrics 一样受 固定/stale-generation gate 保护；
- explicit reentry：同线程嵌套读取被 EDEADLK 拒绝并清零输出；
- installed 消费方：使用安装后的 公开头文件/library 实际调用 诊断。

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
不在支持契约内。支持边界是 延迟取消，或调用前已禁用 取消。
restore-policy 回归 会验证原先 disabled 的 调用方 仍保持 disabled 且 取消 type
不被破坏；构造函数 契约 会拒绝 enabled asynchronous 取消。
