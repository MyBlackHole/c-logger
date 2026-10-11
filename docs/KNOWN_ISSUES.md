# 当前开发限制与生产验收边界

本页针对 main 的 Audit-free ABI 2 开发产物。旧 Production v1 验收和限制保存在
[历史记录](V1_KNOWN_ISSUES_HISTORY.md)，不作为当前版本已通过的证据。

## 尚未完成的发布事项

- 根 VERSION 暂为1.0.0，ABI已切至SONAME 2 / LOGGER_2.0；开发包不得作为v1兼容产物发布。
- 2.0公开API/ABI最终冻结、发布触发策略和文档收敛尚需完成。CI通过不等于正式release授权。
- 历史Audit源码/测试仍存在于未安装的private/regression支持库中；普通生产目标始终不包含Audit，测试开关不得改变生产制品。
- 每次生产候选必须在精确提交上验证GCC/Clang、shared/static、ASan/UBSan、TSan、安装消费、制品隔离和平台门禁。旧提交上的成功不能移用。

## 运行与部署边界

- 平台限定Linux x86_64/ELF64、kernel >=5.10、glibc >=2.31；GCC >=9、Clang >=10，公共头文件支持C++11。musl、AArch64和32位不在当前支持范围。
- 宿主拥有显式logger实例。销毁/卸载前必须停止并join所有借用者；原始指针不支持并发destroy、跨静态库副本使用或销毁后重试。
- 文件目录必须可信，文件末级symlink/hardlink被拒绝。锁只是协作协议，不抵抗不合作writer、恶意目录/锁inode替换；不支持NFS/SMB等网络文件系统。
- 内部轮换要求RENAME_NOREPLACE，不做覆盖式降级。错误可能发生在部分字节已写入后，业务重试可能重复；检查flush/destroy状态与粘滞错误。
- Syslog仅支持本地非阻塞Unix datagram；不提供磁盘重试队列、失败记录重放、远端持久确认或绝对I/O时限。重连仅由后续日志触发。
- Logger/Console不是信号安全API。raw fork继承运行时拒绝使用，优先fork+exec；不支持vfork后调用、跨调用longjmp/pthread_exit、宿主取消私有worker或回调等待环。
- 支持延迟取消或入口前已禁用取消；ASYNC+ENABLE不在一般API支持契约内。取消延期期间底层文件/stdio和锁操作仍可能阻塞。
- Logger的stderr与bootstrap路径保护调用线程免受本次写入新产生SIGPIPE的默认终止，保留宿主handler、mask和既有pending信号。Console的stdio管道行为另由公开Console合同定义，不能由Logger测试推导。
- 普通日志不提供审计事务、哈希链、不可变存储或可信锚点。

## 持久性证据范围

进程crash和QEMU/raw ext4+XFS掉电测试验证受控虚拟存储路径，不证明物理控制器/磁盘易失缓存、
RAID/HBA或具体部署介质的断电安全。必须保留普通文件轮换切断点；历史Audit测试的成功不替代普通Logger验证。
最低kernel/glibc要有实际guest/runtime证据，不能仅从ELF符号版本推导kernel兼容性。

当前设计权威与支持细则见 [v2/README.md](v2/README.md)、[v2/SUPPORT_MATRIX.md](v2/SUPPORT_MATRIX.md)。
