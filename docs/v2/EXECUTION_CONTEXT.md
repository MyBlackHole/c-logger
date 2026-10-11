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

<a id="init"></a>

### 初始化（INIT）

对象尚未发布。允许构造资源，但失败必须完整回滚。

<a id="public_api"></a>

### 公开 API（PUBLIC_API）

由宿主线程调用。是否可阻塞、是否属于热路径由第二维标签决定。

<a id="worker"></a>

### 工作线程（WORKER）

库自己的后台执行者。允许执行已明确授权的阻塞 I/O，但不能因此绕过关闭/静止状态协议。

<a id="callback"></a>

### 回调（CALLBACK）

由外部或内部回调驱动。必须明确重入和锁约束，默认禁止形成反向等待。

<a id="teardown"></a>

### 销毁（TEARDOWN）

对象已经关闭准入或进入停止路径。只允许销毁契约中定义的操作。

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
- HOT_PATH 禁止无界分配、无界等待和未证明的同步开销。
- NO_ALLOC 上下文不得通过辅助函数间接分配。
- CALLBACK 必须明确是否 REENTRANT；未声明时按不可重入设计。
- TEARDOWN 不应为了完成清理再依赖大规模、可能失败的新分配。
- 每个新阻塞调用在审查中必须回答“为什么当前上下文允许阻塞”。

## 时间规则

- 墙上时钟日志时间：`CLOCK_REALTIME`。
- 超时/截止时间/重试/冷却：`CLOCK_MONOTONIC`。
- 优先绝对单调时钟截止时间，避免反复计算相对超时。
- ns/us/ms 转换必须防止溢出。

## 栈规则

- 工作线程/公开 API/回调都必须有受控栈占用。
- 禁止大尺寸局部数组和深递归。
- 大块临时存储使用有界堆/工作区。
- #101 关闭前建立栈帧大小编译门禁。
