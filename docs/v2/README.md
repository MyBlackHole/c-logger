# c-logger 2.0 设计权威

本目录是当前不含 Audit 的 ABI 2 的唯一设计入口。根目录专项文档补充普通 Logger 的实现约束；
旧 Audit/密码学模块设计不是当前对象模型或验证证据。移除边界见 [AUDIT_REMOVAL.md](AUDIT_REMOVAL.md)。


## 设计顺序

2.0 必须坚持：

```text
需求
    ↓
对象模型
    ↓
所有权图
    ↓
生命周期模型
    ↓
并发模型
    ↓
选择原语
    ↓
实现
    ↓
验证
    ↓
公开 API/UAPI/ABI 冻结
```

禁止先实现引用计数、RCU、seqcount 等原语，再寻找生产使用场景。

## 文档职责

- [SUPPORT_MATRIX.md](SUPPORT_MATRIX.md)：平台、工具链与构建支持边界。
- [CODING_STYLE.md](CODING_STYLE.md)：Linux 风格 C 编码与编译器规则。
- [EXECUTION_CONTEXT.md](EXECUTION_CONTEXT.md)：执行位置、阻塞/分配/热路径约束。
- [ARCHITECTURE.md](ARCHITECTURE.md)：当前 2.0 对象/状态机设计审查基线，定义所有权、发布、退役、错误与后端边界（#104）；不代表获批发布。
- [INVARIANTS.md](INVARIANTS.md)：2.0 不变量，随各子计划逐步冻结。
- [OWNERSHIP.md](OWNERSHIP.md)：所有权/生命周期图，主要由 #102 维护。
- [LOCKING.md](LOCKING.md)：锁职责、顺序与 lockdep 风格的契约，由 #103 维护。
- [GLOBAL_R7_PROOF.md](GLOBAL_R7_PROOF.md)：控制器候选/退役对象阶段与最终释放的独立证明（#199）。
- [MEMORY_MODEL.md](MEMORY_MODEL.md)：C11 内存模型、发布与无锁访问证明，由 #103 维护。
- [LOCKDEP.md](LOCKDEP.md)：当前六种锁类的调试/测试检查与边界。
- [SEMANTIC_FAILURE_POLICY.md](SEMANTIC_FAILURE_POLICY.md)：生产资源获取清单、同步语义门禁、故障注入矩阵及误报边界（#106）。
- [FUZZ_PROPERTY_POLICY.md](FUZZ_PROPERTY_POLICY.md)：当前公开配置/脱敏有界模糊测试与 FIFO/溢出池属性模型（#107）。
- [RELEASE_PREFLIGHT.md](RELEASE_PREFLIGHT.md)：最终发布前源码身份、现有 CI 证据与尚待维护者签收的边界，不是发布授权。
- [FD_OWNERSHIP.md](FD_OWNERSHIP.md)：文件候选 fd 的拥有/借用类型及 fork 资源位图合同，非完整 D1/D2 结项。
- [ENUM_ATOMIC_TYPES.md](ENUM_ATOMIC_TYPES.md)：实例状态和日志级别十处原子写入的私有参数约束、编译对照与 CAS 写回边界。
- [QUEUE_WAKEUP_PROOF.md](QUEUE_WAKEUP_PROOF.md)：队列唤醒的 C11 证明。
- [LOCKLESS_PROOF_TEMPLATE.md](LOCKLESS_PROOF_TEMPLATE.md)：新增无锁访问协议的审查模板。
- [WORKER_STARTUP.md](WORKER_STARTUP.md) / [WORKER_FAILURE.md](WORKER_FAILURE.md)：工作线程启动与失败退出合同。
- [架构不变量](../ARCHITECTURE_INVARIANTS.md)：普通 Logger 的重构/扩展门禁。

本目录其余 `AUDIT_*.md`（除迁移说明 [AUDIT_REMOVAL.md](AUDIT_REMOVAL.md)）仅为已退役实现的历史记录，不安装、不构成现行设计权威。

## 当前状态

2.0.0 已正式发布：[公开 ABI 合同](../../abi/LOGGER_2_0_CONTRACT.md)
依据43个公开符号及C/C++可执行快照冻结，正式源码与发布包由 `v2.0.0` 标签定位。
记录与状态/指标的当前合同分别见[记录接口](RECORD_API.md)和[查询接口](QUERY_API.md)。
本合同不新增引用计数、RCU 或无锁（lock-free）进度保证。

对应路线图：#100。工程基线：#101。
