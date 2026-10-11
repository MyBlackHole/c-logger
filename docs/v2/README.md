# c-logger 2.0 设计权威

本目录是当前 Audit-free ABI 2 的唯一设计入口。根目录专项文档补充普通 Logger 的实现约束；
旧 Audit/crypto 设计与已发布 v1 记录不是当前对象模型或验证证据。移除边界见 [AUDIT_REMOVAL.md](AUDIT_REMOVAL.md)。

Production v1 的已发布行为、ABI 与历史设计仍由现有 v1 文档、`release/1.x` 和 `v1.0.0` tag 定义。2.0 不要求兼容这些内部设计或 public ABI。

## 设计顺序

2.0 必须坚持：

```text
requirements
    ↓
object model
    ↓
ownership graph
    ↓
lifetime model
    ↓
concurrency model
    ↓
choose primitive
    ↓
implementation
    ↓
verification
    ↓
public API/UAPI/ABI freeze
```

禁止先实现 refcount、RCU、seqcount 等 primitive，再寻找生产使用场景。

## 文档职责

- [SUPPORT_MATRIX.md](SUPPORT_MATRIX.md)：平台、工具链与构建支持边界。
- [CODING_STYLE.md](CODING_STYLE.md)：Linux 风格 C 编码与编译器规则。
- [EXECUTION_CONTEXT.md](EXECUTION_CONTEXT.md)：执行位置、阻塞/分配/热路径约束。
- [ARCHITECTURE.md](ARCHITECTURE.md)：2.0 总体组件与对象模型，随 #104 收敛。
- [INVARIANTS.md](INVARIANTS.md)：2.0 不变量，随各子计划逐步冻结。
- [OWNERSHIP.md](OWNERSHIP.md)：ownership/lifetime 图，主要由 #102 维护。
- [LOCKING.md](LOCKING.md)：锁职责、顺序与 lockdep-style contract，由 #103 维护。
- [MEMORY_MODEL.md](MEMORY_MODEL.md)：C11 memory model、publication 与 lockless proof，由 #103 维护。
- [LOCKDEP.md](LOCKDEP.md)：当前六种锁类的 debug/test 检查与边界。
- [QUEUE_WAKEUP_PROOF.md](QUEUE_WAKEUP_PROOF.md)：queue wakeup 的 C11 proof。
- [LOCKLESS_PROOF_TEMPLATE.md](LOCKLESS_PROOF_TEMPLATE.md)：新增 lockless protocol 的 review 模板。
- [WORKER_STARTUP.md](WORKER_STARTUP.md) / [WORKER_FAILURE.md](WORKER_FAILURE.md)：worker 启动与失败退出合同。
- [架构不变量](../ARCHITECTURE_INVARIANTS.md)：普通 Logger 的重构/扩展门禁。

本目录其余 `AUDIT_*.md`（除迁移说明 [AUDIT_REMOVAL.md](AUDIT_REMOVAL.md)）仅为已退役实现的历史记录，不安装、不构成现行设计权威。

## 当前状态

当前阶段只冻结工程基线，不冻结 2.0 public API/ABI，也不预先决定 Logger 是否 refcounted、Global 是否使用 RCU、Queue 是否 lock-free。

对应路线图：#100。工程基线：#101。
