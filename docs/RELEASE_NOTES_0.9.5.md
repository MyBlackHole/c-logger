# c-logger 0.9.5

0.9.5 是 0.9.4 之后的**受控生产发布候选平台验证版本**。本版没有修改 `src/` 或
`include/` 运行时代码；重点是把 XFS 掉电恢复、旧 glibc userland、发布失败恢复与
老工具链兼容纳入可执行门禁。

仍不是完成真实硬件、全部目标平台和安全认证的通用 Production v1。

## 变化

### ext4 + XFS power-cut

- QEMU/raw-disk 10-point power-cut matrix 从 ext4-only 扩展为 ext4 + XFS；
- 两种文件系统使用相同的：
  - acknowledged cut；
  - Audit fsync 前/后；
  - checkpoint rename 前/后与 commit 后；
  - file rotation archive rename/dirsync 与 active open/dirsync；
- 写入阶段观察指定 serial marker 后，由 host 对 QEMU 执行 SIGKILL；
- 使用同一 raw disk 重新启动 guest，执行恢复、追加与完整 Audit chain verify；
- serial evidence 显式记录 guest kernel release 与 mounted filesystem；
- XFS 为模块时，将所选 guest kernel 对应的 XFS module/dependency 打入 initramfs。

这仍不等价于物理服务器断电或 controller/device volatile cache 验收。

### glibc runtime matrix

新增 x86_64 userland 运行矩阵：

- Ubuntu 20.04 / glibc 2.31；
- Ubuntu 22.04 / glibc 2.35；
- Ubuntu 24.04 / glibc 2.39。

每个 userland 都验证：

- shared/static production-linked core tests；
- production artifact isolation；
- installed C consumer；
- installed C++11 consumer；
- pkg-config consumer；
- shared ELF ABI / SONAME / public symbol set；
- shared ELF 的 GLIBC requirements 不高于当前容器实际 glibc。

容器共享 GitHub runner kernel，因此该矩阵建立的是已验证 glibc/userland 范围，
**不建立最低 Linux kernel**。

### 发布恢复

`release-publish` 现在具备幂等恢复语义：

- 发布任务跨 ref 串行，避免并发发布互相覆盖；
- 已存在 tag 时固定使用该 tag 对应 commit 作为恢复源码；
- 校验 tag 中 `VERSION` 与待发布版本一致；
- 已有 release 只有在 prerelease metadata、四个 asset 数量和 SHA-256 全部正确时才跳过；
- release 已存在但不完整时重新构建并使用 `gh release upload --clobber` 修复；
- create/repair 后重新从 GitHub Release 下载四个资产并再次验证。

### 老 userland / packaging 兼容

- root-owned container 中为 Xmake 增加安全 wrapper，正确传递 `--root`；
- ABI 检查显式请求旧 binutils `nm --with-symbol-versions`，并兼容 version node 输出差异；
- pkg-config relocation 测试与“路径包含空格”的 CMake relocation 测试分离，避免把旧
  pkg-config 自身限制错误声明成 package 契约；
- 下游 CMake `find_package(Logger ... CONFIG)` 兼容仍保留。

## ABI 与接入

0.9.5 不改变运行时 public ABI：

- 软件版本：`0.9.5`；
- ABI major：`0`；
- SONAME：`liblogger.so.0`；
- ELF symbol version：`LOGGER_0.9`；
- 默认 public C symbol set：62 项；
- 摘要实现：builtin SHA-256 only。

下游可使用：

```cmake
find_package(Logger 0.9.5 EXACT CONFIG REQUIRED)
target_link_libraries(host PRIVATE Logger::logger)
```

或 pkg-config。

## 发布门禁

0.9.5 发布前要求：

- Xmake production artifact shared/static；
- full regression/private suite；
- ASan + UBSan；
- TSan；
- install/package consumer parity；
- 9-case focused process-crash shared/static × 3；
- ext4 + XFS × 10-point QEMU/raw-disk power-cut；
- Ubuntu 20.04 / 22.04 / 24.04 glibc runtime matrix；
- queue benchmark candidate vs frozen baselines；
- XPack shared/static asset + SHA-256；
- shared ELF ABI / SONAME / symbol allowlist；
- production artifact isolation。

## 验证边界

以下仍不能由当前 CI 替代：

- 实际服务器断电/reset；
- RAID/HBA/NVMe/SATA controller/device volatile cache；
- 实际目标 mount/storage stack；
- 明确选定的最低 Linux kernel；
- 32-bit；
- NFS/SMB；
- 真实 syslogd 环境；
- 安全认证、外部可信签名、远端锚点或 WORM 合规。

详细边界见 `docs/KNOWN_ISSUES.md` 与 `docs/PLATFORM_BASELINE.md`。
