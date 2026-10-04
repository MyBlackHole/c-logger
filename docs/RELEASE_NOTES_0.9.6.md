# c-logger 0.9.6

0.9.6 是 0.9.5 之后的**受控生产发布候选可观测性版本**。本版在保持既有 public API
签名和旧 metrics 结构布局不变的前提下，新增统一 runtime diagnostics 与 File backend
metrics，用于把 queue pressure、spill exhaustion、output failure、fsync/rotation/reopen
等运行时事实暴露给宿主监控系统。

仍不是完成真实硬件、全部目标平台和安全认证的通用 Production v1。

## 变化

### Runtime diagnostics

新增：

- `logger_get_diagnostics()`；
- `logger_get_global_diagnostics()`。

统一快照覆盖：

- 当前 queue depth / capacity 与 high watermark；
- completion backlog；
- queue storage；
- spill in-use / capacity / exhaustion；
- worker waiting / wakeup 状态；
- drop 与 sync fallback；
- output success/failure accounting；
- sticky first output error；
- 已观察到 queue saturation、spill exhaustion、drop、fallback、output failure 等 lifetime facts。

diagnostics 读取不执行 flush/reconnect，也不进行 File/Syslog I/O；通用快照保持
backend-lock-free。宿主应采样这些客观 counters/state，并在自己的 Prometheus、
OpenTelemetry、StatsD 或其他监控系统中定义告警阈值。

### File backend metrics

新增：

- `logger_get_file_metrics()`；
- `logger_get_global_file_metrics()`。

File backend 快照区分：

- 顶层 write operation 与底层 write/writev syscall；
- terminal error 前已经写出的 partial bytes；
- EINTR retry 与最终 write failure；
- data fsync 与 directory fsync attempt/failure；
- rotation attempt/success/failure 与 detached state；
- reopen attempt/success/failure；
- retention deletion；
- 最近一次顶层结果；
- sticky write/sync/rotation/reopen errno。

File metrics 只在读取一致 sink snapshot 时获取现有 `emit_mu`，不会触发
write/fsync/rotation/reopen。

### API / ABI

0.9.6 是 additive pre-1.0 API 更新：

- 软件版本：`0.9.6`；
- ABI major：`0`；
- SONAME：`liblogger.so.0`；
- ELF symbol version：`LOGGER_0.9`；
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

## 发布门禁

0.9.6 发布前要求继续通过：

- Xmake production artifact shared/static；
- complete regression/private suite；
- ASan + UBSan；
- TSan；
- install/package consumer parity；
- shared ELF ABI / SONAME / 66-symbol allowlist；
- production artifact isolation；
- 9-case focused process-crash，shared/static 各重复 3 次；
- ext4 + XFS 各 10-point QEMU/raw-disk power-cut；
- Ubuntu 20.04 / 22.04 / 24.04 glibc runtime matrix；
- queue benchmark candidate vs frozen baselines；
- XPack shared/static asset + SHA-256。

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
