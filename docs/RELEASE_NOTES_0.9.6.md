# c-logger 0.9.6

0.9.6 是 0.9.5 之后的**受控生产发布候选可观测性版本**。本版在保持既有 public API
签名和旧 指标 结构布局不变的前提下，新增统一 运行时诊断 与 文件后端
指标，用于把 队列压力、溢出区耗尽、输出失败、fsync/rotation/reopen
等运行时事实暴露给宿主监控系统。

仍不是完成真实硬件、全部目标平台和安全认证的通用 生产版 v1。

## 变化

### Runtime diagnostics

新增：

- `logger_get_diagnostics()`；
- `logger_get_global_diagnostics()`。

统一快照覆盖：

- 当前 队列深度 / 容量 与 高水位；
- 完成积压；
- 队列存储；
- 溢出区 in-use / capacity / exhaustion；
- worker waiting / wakeup 状态；
- drop 与 sync fallback；
- output success/failure accounting；
- sticky first output error；
- 已观察到 queue saturation、溢出区耗尽、drop、fallback、输出失败 等 lifetime facts。

diagnostics 读取不执行 flush/reconnect，也不进行 File/Syslog I/O；通用快照保持
backend-lock-free。宿主应采样这些客观 counters/state，并在自己的 Prometheus、
OpenTelemetry、StatsD 或其他监控系统中定义告警阈值。

### 文件后端 指标

新增：

- `logger_get_file_metrics()`；
- `logger_get_global_file_metrics()`。

文件后端 快照区分：

- 顶层 write 操作 与底层 write/writev syscall；
- terminal error 前已经写出的 partial bytes；
- EINTR retry 与最终 write failure；
- data fsync 与 directory fsync attempt/failure；
- rotation attempt/success/failure 与 detached state；
- reopen attempt/success/failure；
- retention deletion；
- 最近一次顶层结果；
- sticky write/sync/rotation/reopen errno。

File 指标 只在读取一致 sink snapshot 时获取现有 `emit_mu`，不会触发
write/fsync/rotation/reopen。

### API / ABI

0.9.6 是 additive pre-1.0 API 更新：

- 软件版本：`0.9.6`；
- ABI 主版本：`0`；
- SONAME：`liblogger.so.0`；
- ELF 符号版本：`LOGGER_0.9`；
- public C symbol set：66 项；
- 相对 0.9.5 的 62 项新增 4 个 observability accessor；
- 既有 public 函数签名不变；
- `logger_metrics_t` / `logger_io_metrics_t` 布局不变；
- 摘要实现仍为 builtin SHA-256 only。

下游可使用：

```cmake
find_package(Logger 0.9.6 EXACT CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

或 pkg-config。

## 分发许可

0.9.6 起项目明确采用 **Apache License 2.0** 分发；根目录 `LICENSE` 是权威许可文本，
XPack/install tree 同时携带该文件，packaging gate 会拒绝缺失或错误的许可文件。

## 发布门禁

0.9.6 发布前要求继续通过：

- Xmake 生产 artifact 共享库/静态库；
- complete regression/private suite；
- ASan + UBSan；
- TSan；
- install/package consumer 一致性；
- 共享库 ELF ABI / SONAME / 66-symbol allowlist；
- 生产 artifact isolation；
- 9-case focused process-crash，共享库/静态库 各重复 3 次；
- ext4 + XFS 各 10-point QEMU/原始磁盘 断电；
- Ubuntu 20.04 / 22.04 / 24.04 glibc runtime matrix；
- queue benchmark candidate vs frozen baselines；
- XPack 共享库/静态库 asset + SHA-256。

## 验证边界

当前 CI 仍不能替代：

- 实际服务器断电/reset；
- RAID/HBA/NVMe/SATA controller/device volatile cache；
- 实际目标 mount/storage stack；
- 明确选定的最低 Linux kernel；
- 32-bit；
- NFS/SMB；
- 真实 syslogd 环境；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

详细边界见 `docs/KNOWN_ISSUES.md` 与 `docs/PLATFORM_BASELINE.md`。
