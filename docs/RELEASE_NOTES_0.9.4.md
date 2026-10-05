# c-logger 0.9.4

0.9.4 是 0.9.3 之后的**受控生产发布候选工程收敛版本**。本版不改变 public C API/ABI、
SONAME `liblogger.so.0` 或 `LOGGER_0.9` 符号版本；重点是把已经完成 一致性 的
Xmake 路径正式收敛为唯一项目构建、测试、安装和发布权威，并删除当前源码树中已经退役的
项目级 CMake/CTest/CPack 实现。

仍不是完成真实硬件、全部目标平台、全部文件系统和安全认证的通用 生产版 v1。

## 变化

### Xmake 单一权威

- 生产 共享库/静态库、核心/私有/回归测试、ASan/UBSan/TSan 全部由 Xmake 定义；
- 剩余工具/示例 已补齐 Xmake 目标：
  `bench_logger`、`multi_instance_example`、`syslog_client_example`、
  `crypto_chain_tool`、legacy `fork_reinit_example`；
- release-validation / release-publish 不再依赖 CMake 参考测试集 或 CPack；
- 旧 native CMake CI 删除，Xmake workflow 补齐 `tests/**`、`examples/**` 触发面；
- 根 `CMakeLists.txt` 与 CMake-only packaging/toolchain helper 删除。

### ABI / packaging

- public ABI 允许列表 从 `cmake/logger.symbols` 迁到 `abi/logger.symbols`；
- 默认 public C symbol set 保持 **62 项**，legacy fork helper opt-in 时额外导出
  `logger_fork_reinit`；
- 共享库 SONAME 保持 `liblogger.so.0`，ELF 符号版本 保持 `LOGGER_0.9`；
- Xmake 继续生成可重定位的 `LoggerConfig.cmake`、`LoggerConfigVersion.cmake`、
  `LoggerTargets*.cmake` 和 `logger.pc`；
- `tests/packaging/check_install.py` 直接读取根 `VERSION` 与 `abi/logger.symbols`，
  不再维护第二份版本/ABI 状态；
- release package 仍验证真实 C/C++11 CMake consumer、pkg-config、PIC SDK MODULE、
  ExactVersion/components fail-closed、frozen old-header、ABI/SONAME 与 生产 isolation。

删除的是**项目自身 CMake build system**，不是下游 CMake 消费兼容。

### Crash / 断电 evidence

- focused process-crash 固定为 9 个 Xmake `process-crash` group case；
- 共享库/静态库 各重复 3 次；
- `scripts/check_crash_matrix.py` 从实际 Xmake result log 核对精确集合，并输出
  JSON、JUnit 和原始日志；
- 通用 xmake-一致性 中重复的 9×3 crash job 已删除，避免同一 suite 双跑；
- QEMU 断电 已切为 Xmake-only authoritative guest；
- 10 个 切断点、SIGKILL、原始 ext4 同盘重启、恢复、追加和 Audit chain verify 均保留；
- 在切换 Xmake-only 前，最后一轮 CMake/Xmake dual-build VM matrix 10/10 全部通过。

### Queue benchmark

- 当前 candidate 的 `bench_matrix` 改由 Xmake 构建；
- 两个冻结历史 baseline ref 继续使用各自历史 提交 中原有的 CMake 定义，保证历史结果可重现；
- 比较脚本、线程/消息尺寸矩阵、重复次数和默认 queue 内存下降门槛不变；
- `scripts/benchmark.sh` 与 `crypto_cross_version.py` 已适配 Xmake 的嵌套 binary layout。

### 本地开发入口

- `scripts/check.sh fast`：生产-linked core tests；
- `scripts/check.sh production`：完整 Xmake suite；
- `scripts/check.sh crash`：9 个 process-crash cases；
- `scripts/check.sh legacy-fork`：opt-in legacy helper 回归；
- 历史 `unit/integration/concurrency/reliability/security/host-owned/crypto` 参数保留为
  完整 suite 的保守兼容别名，避免旧命令静默少跑。

## ABI 与接入

- 软件版本：`0.9.4`；
- ABI 主版本：`0`；
- SONAME：`liblogger.so.0`；
- 动态 符号版本：`LOGGER_0.9`；
- 默认 public symbol set：62 项；
- 摘要实现：builtin SHA-256 only。

下游仍可使用：

```cmake
find_package(Logger 0.9.4 EXACT CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

或 pkg-config。业务 SDK 仍推荐使用 宿主-owned Logger / 回调 模型，不跨独立静态副本、
版本或 名称space 传递 `logger_t *`。

## 发布验证

0.9.4 发布前要求以下当前门禁全部成功：

- Xmake 生产 artifact 共享库/静态库；
- install/package consumer 一致性；
- full regression/private suite；
- ASan + UBSan；
- TSan；
- 9-case focused process-crash 共享库/静态库 × 3；
- 10-point QEMU/raw-ext4 断电；
- queue benchmark candidate vs frozen baselines；
- XPack 共享库/静态库 asset + SHA-256；
- 共享库 ELF ABI / SONAME / symbol allowlist；
- 生产 artifact isolation。

## 验证边界

这些结果仍不能等价替代：

- 实际服务器断电/reset；
- RAID/HBA/NVMe/SATA controller/device volatile cache；
- XFS 与实际目标 mount/storage stack；
- 目标最低 kernel/glibc；
- 32-bit；
- NFS/SMB；
- 真实 syslogd 环境；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

详细边界见 `docs/KNOWN_ISSUES.md` 与 `docs/PLATFORM_BASELINE.md`。
