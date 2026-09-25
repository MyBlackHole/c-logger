# c-logger 0.9.3

0.9.3 是 0.9.2 之后的**受控生产发布候选验证更新**。本版不改变 public API/ABI、SONAME
或 `LOGGER_0.9` 符号版本，重点把进程 crash 与 QEMU/ext4 power-cut 验证纳入最新不可变
release tag。仍不是完成真实硬件、全部目标平台及文件系统验收的通用 Production v1。

## 变化

- 新增 Release shared/static 的 focused process-crash recovery matrix，并对已注册 crash
  case 做重复执行，防止测试集合漂移。
- 新增 QEMU guest + raw ext4 同盘重启验证。host 在 guest serial marker 出现后直接
  SIGKILL QEMU，再启动同一虚拟磁盘进行恢复、追加记录和 Audit chain 校验。
- VM 矩阵覆盖 10 个 cut point：
  - acknowledged baseline；
  - `before_audit_fsync` / `after_audit_fsync`；
  - `before_state_rename` / `after_state_rename` / `after_checkpoint_commit`；
  - `file_after_archive_rename` / `file_after_archive_dirsync`；
  - `file_after_active_open` / `file_after_active_dirsync`。
- 修复 VM 测试的 archive filter：当前 active `powercut.audit.log` 不再被 archive scan
  重复校验。
- 发布包 checksum 门禁要求 shared/static 各恰好一个包，并实际校验 SHA-256 与文件名。
- production `logger` target 继续使用 `LOGGER_ENABLE_FAULT_INJECTION=0`；VM crash
  hook 仅由独立 `logger_test_support` 使用，生产构建中调用被预处理宏消除。

## ABI 与接入

- 软件版本：`0.9.3`；ABI major：`0`。
- SONAME：`liblogger.so.0`；动态符号版本：`LOGGER_0.9`。
- 默认 public C symbol set 保持 62 项不变。
- CMake 使用 `find_package(Logger 0.9.3 EXACT CONFIG REQUIRED)`；pkg-config 版本为
  `0.9.3`。
- 宿主继续负责实例 create、停止/join 借用者、flush、destroy，再卸载 SDK。

## 验证边界

当前 QEMU 结果证明的是 GitHub Ubuntu runner 上的 virtual x86_64 + raw ext4 + QEMU
存储路径，在指定切断点后的恢复行为。它不能等价替代：

- 实际服务器断电或 reset；
- RAID/HBA/NVMe/SATA 控制器与设备 volatile write cache；
- XFS 与实际目标 mount/storage stack；
- 最低支持 kernel/glibc、32-bit；
- NFS/SMB；
- 真实 syslogd 环境。

Audit 仍无外部可信签名、远端锚点或 WORM 认证。详细边界见
`docs/KNOWN_ISSUES.md` 与 `docs/PLATFORM_BASELINE.md`。
