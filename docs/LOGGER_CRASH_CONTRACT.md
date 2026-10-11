<a id="ordinary-logger-crash-and-vm-evidence-contract"></a>

# 普通 Logger 崩溃与 VM 证据契约

本文是当前主线契约，取代基于 Audit 的崩溃验收。
不承诺 Audit 事务、检查点、哈希链或自动修复撕裂尾部。
历史发布说明描述旧产品，不予重写。

<a id="what-is-acknowledged"></a>

## 哪些内容已经确认

一个新建、同步、仅输出文件的 `logger_t` 写入十条不同记录，序号为
000–009。每次 `logger_log_sync_status()` 都必须返回成功，随后
`logger_flush_instance_status()` 也必须成功，之后才启用任何切断点。
进程保持打开：不能通过成功销毁来补足基线的持久性。
仅测试使用的固定实时时钟和 UTC 时区，使整条格式化记录确定可复现，
包括时间戳、级别、序号、负载、终止符和换行。单调时钟与截止时间仍使用真实时间。

轮转使用一字节大小阈值，因此每个非空归档恰好包含一条完整记录。
保留策略禁用。启用轮转切断点之前已有九个归档，下一次写入必须执行真正的第十次轮转。
严格按归档名称排序，最后读取活动文件，完整字节流必须按顺序与全部十条不同基线记录
逐字节匹配，每条恰好出现一次。错误负载、重复或缺失记录、不完整归档、
`crash.*` 命名空间内的意外名称、符号链接、硬链接、多余记录和超大文件都会导致验证失败。

恢复首先验证未经修改的文件。随后新建普通 Logger 追加序号 011 并关闭；
再由另一个新实例追加 012 并关闭。每次写入、刷新和销毁都必须成功。
每次打开还检查竞争所有者会收到 `EBUSY`；后续打开成功证明被终止/先前的所有者
已释放其独占租约。每次追加后都检查完整文件清单，包括全部基线和恢复字节。
这是测试判定器针对串行写入者的“恰好一次”检查，
不是应用级事务性的恰好一次交付保证。

<a id="eight-cut-points-applied-to-both-harnesses"></a>

## 两套测试框架都使用的八个切断点

| 切断点 | 实际停止边界 | 被中断的序号 010 |
| --- | --- | --- |
| `acknowledged` | 十条基线写入均已确认，且已显式刷新之后 | 不存在：从未提交 |
| `before_file_fsync` | 紧接目标数据 fd 的真实 `fsync()` 之前 | 不存在、完整，或预期目标内容的精确字节前缀 |
| `after_file_fsync` | 紧接该真实数据 `fsync()` 成功返回之后 | 完整且恰好一次；API 调用尚未返回 |
| `rotation_after_file_fsync` | 真正完成轮转，且目标数据 `fsync()` 成功之后 | 十个不同归档之后，完整且恰好一次 |
| `file_after_archive_rename` | `RENAME_NOREPLACE` 之后的既有后端钩子 | 不存在：轮转先于目标写入；重命名可能持久保留或回滚 |
| `file_after_archive_dirsync` | 归档目录同步之后的既有钩子 | 不存在；十个归档必须保留 |
| `file_after_active_open` | 创建新活动文件之后、其 inode/目录同步之前的既有钩子 | 不存在；十个归档保留，空活动文件可能不存在 |
| `file_after_active_dirsync` | 新活动文件 inode 和目录同步之后的既有钩子 | 不存在；十个归档及空活动文件必须保留 |

只有测试二进制使用 `--wrap=fsync`、`--wrap=clock_gettime` 和
`--wrap=logger_fault_crash_if_requested`。fsync 包装器比较实际 fd 的普通文件身份
与活动文件 inode，并要求其写后大小精确符合预期。
它不能停在目录 fsync、归档 fd 或轮转期间新建空 inode 的 fsync 上。
轮转加 fsync 场景还要求先到达真正的 `file_after_active_dirsync` 钩子。
生产源码与安装库没有新增钩子。未使用的旧内部 `before_audit_fsync` 钩子
已移除；上述八个切断边界不变。

未经确认的 fsync 前目标可能丢失或被截短。只接受其精确预期内容的字节前缀，
不接受任意扇区损坏或无关垃圾数据。其尾部不必以换行结束：验证使用字节长度
和完整恢复后缀，不按行切分。普通 Logger 不修复该撕裂尾部，
下一条格式化记录可能紧邻它。本测试要求新记录的完整字节被追加，
不要求 Audit 式记录挽救。其他物理损坏模型需要独立验收。

<a id="matrix-migration-not-silent-coverage-reduction"></a>

## 测试矩阵迁移，不静默缩减覆盖

| 旧证据 | 新证据或明确退役 |
| --- | --- |
| VM `acknowledged` | 普通 Logger 的 `acknowledged`，现在也覆盖进程 SIGKILL |
| VM `before_audit_fsync` | 普通 Logger 实际的 `before_file_fsync`，现在也覆盖进程 SIGKILL |
| 进程/VM `after_audit_fsync` | 普通 Logger 实际的 `after_file_fsync` |
| 进程 `rotation_crash_sha256` | 普通 Logger 的 `rotation_after_file_fsync`，现在也覆盖 VM |
| 四个 `file_crash_sha256_file_after_*` / VM 轮转点 | 保留相同四个真实文件后端钩子；使用完整普通 Logger 字节判定器 |
| `before_state_rename` | 退役：Audit 检查点事务在普通 Logger 中没有对应机制 |
| `after_state_rename` | 退役：Audit 检查点事务在普通 Logger 中没有对应机制 |
| `after_checkpoint_commit` | 退役：Audit 检查点事务在普通 Logger 中没有对应机制 |

旧进程用例集有九项：四项 Audit fsync/检查点、一项 Audit 轮转，以及四项文件轮转。
移除三项仅用于检查点的场景，再增加已确认/fsync 前的进程检查后，共八项。
旧 VM 用例集有十项：移除这三个仅用于检查点的点，再增加轮转后的 fsync 点后，共八项。
`scripts/crash_contract.py` 在两种 JSON 证据格式中记录此映射。
历史 Audit 轮转及 busy/cwd 测试已另行退役；普通 `process-crash` 分组不变。

<a id="distinct-failure-boundaries-and-evidence"></a>

## 不同的失败边界与证据

- 进程：新 exec 的写入进程通过管道报告精确到达的切断点，控制器随后才发送
  `SIGKILL`。标记错误/缺失或十秒超时都判失败，即使清理过程终止了写入进程也一样。
  控制器要求实际的 `WIFSIGNALED` / `SIGKILL` 状态。内核及其页缓存仍然存活。
- VM：客户机向串口控制台打印精确切断点后等待，不清理，也不执行客户机同步。
  宿主必须收到该行，随后用 SIGKILL 终止 QEMU，验证信号返回码，
  再使用同一个原始磁盘重启（`cache=none`）。恢复启动必须打印成功标记，
  客户机程序以零退出，QEMU 也必须成功关闭。客户机页缓存会在终止时丢失。
- VM CI 在原始 ext4 与 XFS 上都运行完整八个切断点，包括 PR。
  串口日志记录客户机内核/文件系统及每次文件清单结果；`vm-matrix.json`
  记录所选切断点、迁移、阶段标记和 QEMU 结果。
- 进程 CI 保留既有共享/静态构建配置矩阵和重复策略
  （PR：共享配置一次；main/手动：两种配置各三次）。两种配置都有意执行私有静态支持归档，
  不是通过生产共享 DSO 注入故障。精确用例集检查输出 JSON 和 JUnit；
  缺失用例不能被判为通过。

在 OverlayFS 上本地执行只测试进程恢复及判定器，不能建立 ext4/XFS VM 持久性证据。
QEMU/原始磁盘证据也不是物理 PDU 断电、存储控制器易失缓存测试，
或任意服务器/NVMe/SATA/RAID/HBA/挂载组合的验收。所需的目标存储验收仍应独立完成。

<a id="commands"></a>

## 命令

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build --build_shared=n --build_private_tests=y
xmake test -g process-crash -j1 -vD | tee crash.log
python3 scripts/check_crash_matrix.py crash.log --output crash-matrix.json --junit-dir .
xmake test -g crash-verifier -j1 -vD
xmake vm_powercut_guest
```

独立的判定器变异测试覆盖每一种允许的 fsync 前字节前缀长度，分别带有和不带恢复后缀；
还覆盖基线、恢复记录、归档内容的缺失/重复/篡改，以及恶意命名空间测试样本。
它还检查 Xmake 和完整 VM 矩阵是否与声明的切断点集合一致。
这是判定器检查，不能代替真正终止进程或 VM。
