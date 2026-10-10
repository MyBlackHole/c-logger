# c-logger 2.0 设计权威

本目录是 c-logger 2.0 开发的设计 authority。

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

- `SUPPORT_MATRIX.md`：平台、工具链与构建支持边界。
- `CODING_STYLE.md`：Linux 风格 C 编码与编译器规则。
- `EXECUTION_CONTEXT.md`：执行位置、阻塞/分配/热路径约束。
- `ARCHITECTURE.md`：2.0 总体组件与对象模型，随 #104 收敛。
- `INVARIANTS.md`：2.0 不变量，随各子计划逐步冻结。
- `OWNERSHIP.md`：ownership/lifetime 图，主要由 #102 维护。
- `LOCKING.md`：锁职责、顺序与 lockdep-style contract，由 #103 维护。
- `MEMORY_MODEL.md`：C11 memory model、publication 与 lockless proof，由 #103 维护。
- `AUDIT_TRANSACTION_SEMANTICS.md`：当前 Audit 事务字段的关联语义、验证边界与未来严格配对的前置条件。

## 当前状态

当前阶段只冻结工程基线，不冻结 2.0 public API/ABI，也不预先决定 Logger 是否 refcounted、Global 是否使用 RCU、Queue 是否 lock-free。

对应路线图：#100。工程基线：#101。
