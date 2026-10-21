# Audit 退役边界

c-logger 2.0 只提供普通日志、Console、文件轮转与本地 Syslog。生产库、安装头文件和测试支持库均不包含 Audit 或密码学模块，也不导出 `audit_*` 符号。

当前 ABI 为 SONAME 2 / `LOGGER_2.0`，公开符号清单见 [2.0 ABI 合同](../../abi/LOGGER_2_0_CONTRACT.md)。发布包与安装消费测试只按当前 ABI 验收。

普通日志不提供事务审计、哈希链、检查点恢复、不可变存储或可信锚点。需要这些能力的部署应接入独立审计组件。普通 Logger 的写入故障、轮转、资源回收与进程终止回归继续由 [测试策略](../../TESTING.md) 和 [崩溃合同](../LOGGER_CRASH_CONTRACT.md) 验证。
