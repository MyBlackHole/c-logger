# c-logger 2.0 Memory Model

状态：**由 #103 设计，当前未冻结具体 publication protocol**。

2.0 的用户态并发首先服从 ISO C11 memory model。

关键原则：

- `volatile` 不是同步。
- 不能直接复制 kernel-only `READ_ONCE/WRITE_ONCE` 语义来允许普通对象 data race。
- 共享并发状态使用 C11 `_Atomic`、锁或其他满足 C11 的同步。
- acquire/release publication 必须明确 payload writer、publish point、observer 和 happens-before edge。
- lockless/seqcount/RCU 设计必须单独证明 linearization、reclamation、ABA 与 progress。
