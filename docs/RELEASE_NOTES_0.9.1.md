# c-logger 0.9.1

0.9.1 是在 0.9.0 发布工程基础上的**受控生产发布候选**。本版保持 public C ABI、
`liblogger.so.0` SONAME 与 `LOGGER_0.9` 符号版本不变，重点收敛异步日志热路径、
资源生命周期和架构可维护性。

## 主要变化

- async queue 改为 compact slot + bounded spill pool，默认 8192 queue 固定内存相对
  pre-compact 基线下降约 63%；
- inline 正文提升到 512B，long-message spill freelist 改为 atomic bitmap；
- self-paced worker wakeup：backlog 场景避免每条 enqueue 都执行 mutex + cond signal；
- producer metadata 按 `detail/include_*` 需求采集，跳过最终不会输出的 context/source 等字段；
- PID 在实例创建时缓存，保留按需 TID 获取；
- Linux-style ownership/cleanup、Lock Matrix、Architecture/Architecture Invariants 文档完成收敛；
- cancellation regression 改为稳定 queue boundary，不再依赖可选 cond-signal 实现细节；
- benchmark、Release/Debug、ASan/UBSan、TSan 与 package validation 保持为发布门禁。

## ABI / 集成

- 软件版本：`0.9.1`
- ABI major：`0`
- shared SONAME：`liblogger.so.0`
- shared file：`liblogger.so.0.9.1`
- symbol version：`LOGGER_0.9`
- public symbol set：保持 0.9.0 的 62 个默认符号不变
- CMake：`find_package(Logger 0.9.1 EXACT CONFIG REQUIRED)`
- pkg-config：`0.9.1`

## 推荐使用方式

生产集成继续推荐 host-owned explicit `logger_t`：

```text
Host create logger
  -> SDK/callers borrow
  -> stop/join all borrowers
  -> flush
  -> logger_destroy_status
  -> dlclose SDK / process exit
```

业务 SDK 不应在 constructor/destructor 中偷偷初始化或关闭 global Logger。

## 当前边界

0.9.1 **不是通用 Production v1**。以下仍需在实际目标环境验收：

- 最低支持 kernel / glibc / architecture 矩阵；
- 真实 ext4/XFS 与目标存储栈的 crash/power-loss durability；
- NFS/SMB；
- 32-bit；
- 真实 syslogd 环境；
- Audit 外部可信签名/锚点/WORM 能力不在当前实现范围。

详细说明见 `docs/RELEASE_ENGINEERING.md`、`docs/PLATFORM_BASELINE.md` 和
`docs/KNOWN_ISSUES.md`。
