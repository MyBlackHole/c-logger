# c-logger 2.0 执行上下文规范

执行上下文分成两个正交维度：**执行位置**和**执行能力/约束**。禁止把二者混成单一枚举。

## 执行位置

```text
INIT
PUBLIC_API
WORKER
CALLBACK
TEARDOWN
```

### INIT

对象尚未 publish。允许构造资源，但失败必须完整 rollback。

### PUBLIC_API

由宿主线程调用。是否可阻塞、是否属于 hot path 由第二维标签决定。

### WORKER

库自己的后台执行者。允许执行已明确授权的阻塞 I/O，但不能因此绕过 shutdown/quiescence protocol。

### CALLBACK

由外部或内部 callback 驱动。必须明确 reentry 和锁约束，默认禁止形成反向等待。

### TEARDOWN

对象已经关闭 admission 或进入停止路径。只允许 teardown contract 中定义的操作。

## 能力与约束

```text
MAY_BLOCK
MUST_NOT_BLOCK
MAY_ALLOC
NO_ALLOC
HOT_PATH
REENTRANT
```

这些标签可以组合。例如：

```text
PUBLIC_API + HOT_PATH + NO_ALLOC + MUST_NOT_BLOCK
WORKER + MAY_BLOCK + MAY_ALLOC
CALLBACK + MUST_NOT_BLOCK + NO_ALLOC
```

## 规则

- 只有 MAY_BLOCK 上下文才能进行条件等待、不可控文件系统 I/O 或可能长期等待的 pthread 操作。
- HOT_PATH 禁止无界 allocation、无界等待和未证明的同步开销。
- NO_ALLOC 上下文不得通过 helper 间接分配。
- CALLBACK 必须明确是否 REENTRANT；未声明时按不可重入设计。
- TEARDOWN 不应为了完成清理再依赖大规模、可能失败的新分配。
- 每个新 blocking call 在 review 中必须回答“为什么当前上下文允许阻塞”。

## 时间规则

- wall-clock 日志时间：`CLOCK_REALTIME`。
- timeout/deadline/retry/cooldown：`CLOCK_MONOTONIC`。
- 优先 absolute monotonic deadline，避免反复计算 relative timeout。
- ns/us/ms 转换必须 overflow-safe。

## 栈规则

- worker/public/callback 都必须有受控 stack footprint。
- 禁止大尺寸局部数组和深递归。
- large scratch 使用有界 heap/workspace。
- #101 关闭前建立 frame-size 编译门禁。
