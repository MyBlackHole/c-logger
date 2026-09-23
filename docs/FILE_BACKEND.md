# 文件后端契约：单 owner / 固定目录 / 不覆盖轮换

本版在 Host-owned 版本上加固文件资源，不接管第三方进程或线程模型。
这是开发版兼容性收紧，不是 Production v1 宣告。

## 支持范围和兼容性变化

- `LOGGER_OUT_FILE` 的普通文件目标现在只有一个协作 owner。无论 NONE、EXTERNAL
  还是内部轮换模式，第二个独立实例都被拒绝；共享目标应共享同一个 `logger_t *`。
- 独占锁是 `<active basename>.logger.lock` 上的非阻塞 Linux `flock`。
  同进程不同 `open`、跨进程及父目录路径别名都会争用同一个锁 inode。
  冲突返回 `EBUSY`。不使用全局对象注册表，也不使用会受同进程其他 fd close
  影响的 POSIX record lock 来实现这个普通文件 owner。
- 常驻锁文件创建权限请求为 0600（仍受宿主 umask 约束）。创建普通日志现在需要
  创建/访问协调文件的权限，不再只要求对已有 active 文件具有追加权限。
  库从不 unlink 协调文件；运维不得在 owner 存活时删除/替换它。
- active 和协调文件最终路径不允许符号链接或硬链接（`ELOOP` / `EMLINK`）；目录
  祖先可在初始化时经过 symlink 解析，之后绑定到实际目录 fd。同一个目录的不同
  spelling 不会创建不同 owner。
- 内部轮换仅支持普通文件。FIFO、socket、目录等拒绝；字符设备仅 NONE 模式接受，
  用于如 `/dev/null`、`/dev/full` 管线测试，不是持久化保证，fsync 仍可能返回 EINVAL。
- 空路径、末尾 slash / `.` / `..`、过长路径/文件名返回错误，绝不截断再打开。
  basename 还须为 `.logger.lock` 与内部轮换的 24 个字符扩展留出空间。
- 操作者必须拥有和信任日志目录及祖先；整个目标的归档命名空间归同一个 owner 管理。
  不允许把另一个 Logger 的 active 故意命名为本目标的归档文件。

**协作锁不是安全防篡改机制。** 不支持忽略协议的 writer、恶意替换 inode、绕过锁的
外部写入、跨版本不遵守此锁协议的 writer，也没有验收 NFS/SMB 的分布式锁语义。
子进程继承的 flock 描述符引用可能延长锁寿命，直到全部引用关闭；CLOEXEC 会在 exec
时关闭，不表示 fork 就关闭。现有 raw-fork guard 不在本轮放宽。

## 固定目录，不跟随后来的 cwd

普通文件 init 时打开父目录 `O_DIRECTORY|O_CLOEXEC`，保存 basename。后续 open、reopen、
rename、retention 和目录同步使用同一个 dirfd；宿主后续 `chdir()` 或父目录改名，不会
将日志切换到另一个同名目录。应用不应在初始化正在解析多个配置路径时并发改变 cwd。
原始完整 path 只用于诊断，不用于重新解析文件操作。

内部轮换前核对 active fd 和目录项的 dev/ino。active 被外部替换时返回 ESTALE，不
归档另一个文件。使用外部 logrotate 时，应选 EXTERNAL 并由宿主安排 reopen，不要让
内部轮换和外部工具并发管理同一个 active。

## 时间归档不覆盖

保留已有命名：`xxx.<UTC时间>.log`、`xxx.audit.<UTC时间>.log`；不改成 `.1/.2`。
归档调用 Linux `renameat2(..., RENAME_NOREPLACE)` 系统调用：目标存在时 EEXIST，绝不
覆盖。相同时间或时钟回拨，在当前实例最后成功候选之后以微秒递增重试，最多 1024 次。
文件名时间因此是用于唯一命名的逻辑时间候选，不保证等于精确的 wall-clock 事件时间。
达到上限返回 EEXIST，不忙等、不退回普通 rename。

该能力要求内核及文件系统支持 RENAME_NOREPLACE。系统调用不存在/flag 不支持时返回
ENOTSUP；没有覆写式 rename 或 link/unlink 的静默 fallback。使用 syscall 避免引入
新的 glibc renameat2 wrapper 版本依赖，但**不表示所有 Linux 3.x 文件系统已兼容**。
创建 Logger 成功不代表所在文件系统支持将来的内部轮换；不支持的平台应禁用内部轮换，
使用宿主协调的 EXTERNAL，或显式处理轮换失败。

轮换顺序：

1. 确认 owner/active 身份；同步旧数据及尚未确认的目录状态。
2. 不覆盖地归档旧 active；标记旧 fd 已 detached，之后禁止向它追加。
3. 同步归档目录；以 O_EXCL 创建新 active。
4. 同步新空文件与目录后切换到新 fd，关闭旧 fd。
5. 按严格名字、普通文件/单链接规则做 retention，并同步删除结果。

步骤 2 后发生错误：保留旧 fd 供清理/诊断，但它已属于归档，后续写调用拒绝，不能把
历史归档继续当 active。普通 Logger 可以由宿主显式 reopen 恢复输出位置；旧的 sticky
I/O 错误不会被抹掉。Audit 仍按 IO_FAILED 策略停写，不开放就地重试受保护业务。

这些步骤不是跨文件事务。归档成功后新 active 打开失败，目录结构可能已改变；返回
失败不表示所有文件操作回滚。测试的 `_exit` 模拟进程退出，不等价于真实掉电测试。

## Reopen

- 先 open 和验证新候选 fd，失败保留原 fd、大小及身份。
- 成功候选需在切换前确认旧文件同步和目录同步；失败仍不关闭旧 fd。
- 成功切换后才 close 旧 fd。close 失败返回错误，但新 fd 已安装；不重试旧 fd 号码。
- 新文件可能在稍后同步失败前已被创建，不能承诺 reopen 失败无任何目录副作用。
- 失败记录进实例 first_error；后续 reopen 或日志输出成功不会清除历史错误。
- 最终 dispose 无论哪一步失败，都继续关闭 data、owner、dir 三个 fd，保留首个错误。
  显式 `logger_destroy_status()` 仍在普通错误下释放对象，不可重用指针。

## Audit 联动

Audit 先 reserve active 的普通 owner，然后才读取、核验或修复历史；恢复不会先碰到
别的普通 Logger 已持有的 active。reserve 没有打开/创建 active，保证拒绝恢复或
算法失败不被提前的 active 创建掩盖。恢复成功后把 reservation 移进同步 Logger，
不存在 release/reacquire 的 writer 窗口。

checkpoint 目标也有独立 `.logger.lock` reservation；不同名字共享一个显式 state
路径时会冲突。原 `.audit.lock` 的 POSIX 协作锁保留，与既有部署/测试兼容。

本轮保留恢复/checkpoint 的 path 接口，以 `/proc/self/fd/<owned-dirfd>/...` 连接到
持有的目录。默认 state 和自定义 state 都在本生命周期内绑定。**Audit 现在需要可用的
Linux procfs fd 遍历**；不可用时初始化明确失败，不回退到可漂移的相对路径。这不是
/proc/self/task 扫描，不管理宿主线程。chroot/卸载 procfs 等后续变化不在保证范围内。
普通 Logger 文件后端自身不依赖 procfs。

普通文件实例常驻 3 个 fd（data、dir、owner），比之前增加 2 个。启用完整性保护的
Audit 常驻约 7 个文件/目录/锁 fd（此前约 2 个）；checkpoint 临时 I/O 另计。没有
每条日志新增长期资源或新 worker。本轮没有重新测量吞吐，更多轮换同步可能增加延迟。

## 全局便利层的联动修复

Global Logger 创建前先检查 EALREADY，control mutex 串行初始化与完整旧实例处置。
这避免重复 init 先创建候选文件，也避免旧 owner 尚未释放时新 default 抢占文件锁。
本轮不宣称所有 Global API 准入/generation/取消语义都已完成发布审查。

## 参考接口语义

- https://man7.org/linux/man-pages/man2/flock.2.html
- https://man7.org/linux/man-pages/man2/open.2.html
- https://man7.org/linux/man-pages/man2/rename.2.html
- https://man7.org/linux/man-pages/man2/close.2.html

## 本轮验证环境的持久化限制

实际运行目录所在的 OverlayFS 挂载带有 `fsync=volatile`，原始 mount 信息见
`validation/file_backend/environment.txt`。因此本轮通过的是调用顺序、错误传播、
命名空间操作、资源清理和真实进程退出后的恢复验证，**不是介质掉电持久化验收**。
Linux OverlayFS 文档说明 volatile 模式省略对 upper filesystem 的同步，不能保证
系统崩溃后的数据留存。发布前仍需在常规 ext4/XFS 等真实目标文件系统、非 volatile
挂载、可信存储栈上补充 VM/设备故障或掉电测试。本库不能用更多 fsync 抵消文件系统
主动关闭持久化的挂载策略。

参考：https://docs.kernel.org/filesystems/overlayfs.html#volatile-mount
