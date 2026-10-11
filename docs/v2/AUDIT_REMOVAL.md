# 移除 Audit：分阶段迁移方案

## 决策

c-logger 定位为高可靠 C 日志库，不再负责业务审计事务、哈希链检查点、归档恢复或安全审计数据库功能。保留普通日志的同步/异步写入、文件轮换、Syslog、Console、生命周期与错误传播。

## 当前已实施边界

- 生产logger始终Audit-free；private/regression开关不再把Audit源文件或audit.h注入安装产品。
- SONAME 2 / LOGGER_2.0与47符号清单隔离已发布v1；当前VERSION仍待独立发布身份迁移，不可发布为v1。
- production-linked核心测试与最低内核installed probe已转为普通Logger验证；安装、打包、ABI检查使用ABI2。
- 普通Logger crash/QEMU证据已迁出Audit：共享fixture检查十条唯一已确认baseline、四个真实轮转切点、data fsync前后与重开追加；process/VM各八项。三个纯Audit checkpoint点明确退役，见[完整合同与矩阵映射](../LOGGER_CRASH_CONTRACT.md)。
- Audit/crypto源码、头、纯历史测试和fixture已删除；混合测试中的普通覆盖先迁为Logger专用，两个支持archive只使用logger sources。详见[逐场景映射](../../validation/AUDIT_TEST_RETIREMENT.md)。
- 正式2.0 release仍需最终合同冻结、精确SHA全部门禁和发布授权。本文件后续章节保留迁移过程历史。

## 历史：第一阶段

- 默认生产 Xmake 构建不再编译 Audit 的五个实现文件，不再安装 `audit.h`。
- 独立 C/C++ 安装示例不再引用 Audit。
- 为了避免将数百项旧测试与已发布 v1 行为一起破坏，测试配置及显式 `--legacy_audit=y` 仍编译旧 Audit。它是迁移过渡，不是新的产品功能。
- 没有删除既有审计源码或历史测试，方便审查、回滚和继续拆分。

## 第二阶段：专用 CI 清理

- 删除仅对已退役 Audit 归档恢复容量进行基准测量的 `audit-recovery-capacity` 工作流；该门禁不再代表默认 Logger 的可靠性。
- 其他仍被现有测试构建使用的 Audit 源码、脚本和测试目标暂不删除；必须先解除依赖，不能通过禁用普通 Logger 的验证来完成移除。
- 保留普通 Logger 的文件轮换、写入故障、恢复/生命周期与资源上界验证。此项清理不意味着 Audit 已完全移除。

## 历史计划：正式发布前的迁移清单

1. 处理 ABI：已发布 Production v1 的 SONAME 1 / LOGGER_1.0 包含 Audit 符号。**不能用同一 SONAME 发布删去符号的共享库。** 必须冻结 2.0 新 ABI major，更新符号清单、版本脚本、打包/安装元数据和下游 ABI 测试。
2. 将核心 Logger 与 Audit 的私有测试支持依赖完全拆开；删除仅用于 Audit 的测试目标、脚本和工作流，保留对普通文件后端故障和轮换的测试。
3. 审查普通 Logger 的头文件、故障注入点、文件租约与文档是否存在残留 Audit 耦合。不能仅删除文件名而留下无效行为。
4. 删除 `src/audit*.c`、`src/audit*.h`、`include/audit.h` 及专用测试/样例；保留历史版本的发布分支与标签供旧用户迁移。
5. 以无 Audit 的真实生产构建运行 GCC/Clang、ASan/UBSan、TSan、安装消费测试、ELF ABI 检查、文件后端故障注入和性能基线；不能把旧 Audit 测试成功当成新产品验收。

## 迁移原则

旧用户继续使用已发布的 v1 审计功能，或迁移到独立审计服务/数据库。新 c-logger 只负责日志记录与可观察的持久化结果；业务事务配对、不可变存储、可信锚点由业务系统或独立审计组件承担。

**源码修复PR的合并不等于正式release。** Audit历史支持源码已移除；发布身份与最终合同仍需单独验收。
