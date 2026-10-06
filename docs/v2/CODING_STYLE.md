# c-logger 2.0 Linux C 编码规范

2.0 内部代码采用 Linux 内核开发中的设计与编码纪律，但实现必须符合用户态 GNU C11/POSIX/C11 memory model。

## 基本格式

- K&R brace style。
- tab 表示缩进层级，tab width = 8。
- 80 columns 是 preferred review width，不作为机械硬切断；长 URL、格式字符串、可读性更差的强拆行可以例外。
- 一行一个声明。
- 避免超过约三层的复杂嵌套；优先 early return 或拆分 helper。
- 函数围绕单一职责组织，不能靠“大函数 + 注释分段”代替结构设计。
- 新增代码禁止 VLA。
- trailing variable-size object 使用 C99 flexible array member `[]`，禁止新建 `[0]` / `[1]` 尾数组。
- switch fallthrough 必须显式标记和解释。

## 类型与抽象

- 普通内部 struct 不为隐藏指针而机械 typedef；opaque public object、type-safe scalar、明确抽象例外。
- bytes、records、index、count、generation、sequence、flags、state、deadline 等概念不能全部退化成无语义整数。
- 能用 `static inline` 清楚表达时，不使用复杂 macro。
- macro 不得隐式依赖调用方局部变量。
- `__always_inline` 只用于编译语义确实需要的极小 helper，不作为性能装饰。
- `likely/unlikely` 只有 profile/benchmark 证明后才使用。

## 注释

优先解释：

- ownership；
- lifetime；
- locking；
- blocking context；
- memory ordering；
- persistence/crash invariant；
- error semantics；
- 为什么替代方案不安全。

禁止只复述代码。

## 安全 API

新代码默认禁止：

```text
strcpy
strcat
sprintf
VLA
```

`strncpy` 仅在明确需要固定宽度、并清楚处理 NUL 语义时允许。

格式化必须：

- bounded；
- truncation 可观察；
- format string 经过编译器检查；
- size arithmetic 经过 overflow check。

## 资源与错误

- ownership transfer 使用 typed `take_*`；
- lexical single-owner resource 可以使用 cleanup helper；
- error-bearing finalization、shared lifetime、thread lifetime 保持显式；
- 不允许 cleanup error 覆盖 primary error；
- 不允许以 unsigned wraparound 作为正常控制流。

## 并发

- `volatile` 不是同步、atomic 或 memory barrier。
- 用户态并发必须首先满足 ISO C11 data-race rules。
- atomic/lock/refcount/join/lifetime pin 不能互相替代。
- lockless 设计必须提供 linearization、happens-before、reclamation proof。

## Review 原则

2.0 代码优先“最小充分机制”。不能仅因为 Linux 内核存在某 primitive 就引入；必须先证明 c-logger 存在对应问题。


## 自动化门禁

针对进入 `main` 的 2.0 PR：

- `scripts/check_v2_diff.py` 只检查新增 C/C++ 行，避免为了启用规则而顺手重写 v1 遗留代码；
- 新增代码禁止 `strcpy/strcat/sprintf`；
- 新增 `strncpy` 默认拒绝；确属固定宽度协议字段时必须在同一行标记 `V2_FIXED_WIDTH_OK` 并由 reviewer 审核；
- 新增源码直接写 `__attribute__((always_inline))` 被拒绝，统一走 compiler abstraction；
- `git diff --check` 负责 whitespace 错误；
- VLA 和大栈帧由编译器门禁检查。

这些门禁只约束新改动；清理历史技术债必须独立 PR，不能混入无关功能变更。
