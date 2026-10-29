# c-logger 2.0 Linux C 编码规范

2.0 内部代码采用 Linux 内核开发中的设计与编码纪律，但实现必须符合用户态
GNU C11/POSIX/C11 内存模型。说明文字遵循[项目语言约定](../PROJECT_LANGUAGE.md)。

维护者已要求持续收敛到 Linux 编码规范。固定上游版本、完整文件范围、22 节逐条要求、
起始差异及串行验收见[源码树中的符合性收敛计划](https://github.com/MyBlackHole/c-logger/blob/main/docs/superpowers/plans/2026-10-29-kernel-style-conformance.md)。
计划建立不表示全仓已经符合；最终结论须以准确主线的全量检查和逐条证据为准。

## 基本格式

- 使用 K&R 大括号风格。
- 制表符表示缩进层级，宽度为 8 列。
- 80 列是建议的审阅宽度，不作为机械硬切断；长 URL、格式字符串、可读性更差的强拆行可以例外。
- 一行一个声明。
- 避免超过约三层的复杂嵌套；优先提前返回或拆分辅助函数。
- 函数围绕单一职责组织，不能靠“大函数 + 注释分段”代替结构设计。
- 新增代码禁止 VLA。
- 尾部变长对象使用 C99 柔性数组成员 `[]`，禁止新建 `[0]` / `[1]` 尾数组。
- switch 分支贯穿必须显式标记和解释。

## 类型与抽象

- 普通内部 struct 不为隐藏指针而机械使用 typedef；不透明公开对象、类型安全标量、明确抽象例外。
- 字节数、记录数、索引、计数、代次、序列号、标志、状态、超时期限等概念不能全部退化成无语义整数。
- 能用 `static inline` 清楚表达时，不使用复杂宏。
- 宏不得隐式依赖调用方局部变量。
- `__always_inline` 只用于编译语义确实需要的极小辅助函数，不作为性能装饰。
- `likely/unlikely` 只有性能剖析/基准测试证明后才使用。

### 类型表达与 typedef 兼容边界

普通可访问对象使用具名 `struct` / `enum`。声明写出 `unsigned int` 等完整类型，
不改变 `unsigned long`、固定宽度整数、POSIX 类型或既有字段宽度。
原型保留参数名，优先与实际定义对应；回调直接写出函数指针签名。
函数声明不写多余 `extern`；C++ 的 `extern "C"` 链接块继续保留。
GNU ld `--wrap` 的 `__real_*` 声明属于每个测试翻译单元的链接桥接，
其签名与对应包装器／真实接口核对，既不是库的公开声明，也没有多写 `extern` 关键字。
checkpatch 的 AVOID_EXTERNS 同时会报告这类局部桥接，须按具体声明和链接入口审查。
`tools/diagnostics/asan_fork_probe.c` 的两个 `sanitizer_atfork_*` 计数器则由
`asan_atfork_control.c` 定义；这里的 `extern` 是引用已有对象所必需，不能改成新的定义。
`environ` 直接使用 `<unistd.h>` 在 GNU 特性条件下提供的声明。

以下是 KS-003 对当前自有类型的处理对照。别名移除只改变类型表达，
字段顺序、大小、对齐、枚举值和取得／释放协议必须由同一提交上的编译与回归验证。
不能从重命名推导新的所有权保证。

| 原类型名 | 当前表达／处理 |
|---|---|
| `logger_message_t` | `struct logger_message` |
| `msg_t` | 移除重复别名，使用 `struct logger_message` |
| `logger_ctor_state_t` | `struct logger_ctor_state` |
| `logger_scope_t` | `struct logger_scope` |
| `global_scope_t` | `struct global_scope` |
| `logger_file_t` | `struct logger_file` |
| `logger_syslog_t` | `struct logger_syslog` |
| `logger_queue_record_t` | `struct logger_queue_record` |
| `logger_queue_slot_t` | `struct logger_queue_slot` |
| `logger_queue_spill_t` | `struct logger_queue_spill` |
| `logger_queue_t` | `struct logger_queue` |
| `logger_worker_workspace_t` | `struct logger_worker_workspace`，保留前置声明 |
| `logger_sigpipe_guard_t` | `struct logger_sigpipe_guard` |
| `logger_lock_class_t` | `enum logger_lock_class` |
| `logger_lockdep_entry_t` | `struct logger_lockdep_entry` |
| `logger_lockdep_state_t` | `struct logger_lockdep_state` |
| `arg_t`（bench_logger） | `struct bench_argument` |
| `argument_t`（bench_matrix） | `struct matrix_argument` |
| `tls_lock_arg_t` | `struct tls_lock_arg` |
| `call_result_t` | `struct call_result` |
| `fixture_t`（real_syslogd） | `struct syslog_fixture` |
| `output_fn` | `outputs` 数组直接声明为只读函数指针数组 |
| `example_plugin_t` | `struct example_plugin` |
| `example_log_level_t` | `enum example_log_level` |
| `example_log_record_t` | `struct example_log_record` |
| `example_sdk_options_t` | `struct example_sdk_options` |
| `example_log_fn` | 示例的字段和参数直接使用同一回调签名 |

示例内部类型不属于 liblogger 的安装 API；示例宿主、适配器与插件同步使用同一头文件。
`example_sdk_t` 单独保留：对使用者只有前置声明，实例只能通过示例的 create/run/destroy
接口操作，不能直接访问内部名称或回调存储，属于真实不透明对象。

公开头文件已经发布的类型名另受项目 API/ABI 兼容要求约束。下面逐项保留，
**不把可访问结构体的兼容别名说成内核 CodingStyle 普遍许可的 typedef**，
也不据此允许新增同类内部别名。`logger_t` 是不透明对象；其余是现有接口的兼容保留差异。
消除这些公开名称需要另行批准破坏源码兼容的接口迁移，不能在风格任务中静默进行。

| 保留类型 | 具体接口／兼容理由 | 验证对象 |
|---|---|---|
| `logger_t` | 公开头只有前置声明，宿主通过拥有者槽创建／销毁和接口借用 | 生命周期 C/C++ 消费、当前导出 ABI |
| `logger_level_t` | 记录／设置级别的函数参数及配置字段类型 | 精确函数类型、配置布局、枚举常量 |
| `logger_detail_t` | `logger_config_t.detail` 的公开枚举类型 | 配置布局、默认初始化器 |
| `logger_context_t` | context_set/get 的参数及三个借用字符串字段 | 函数类型、结构布局、上下文合同 |
| `logger_output_t` | 已公开的输出位掩码类型名，调用者可显式声明该类型 | 类型名消费、LOGGER_OUT_* 数值合同 |
| `logger_rotate_mode_t` | 公开 rotation.mode 字段类型 | 轮转配置布局、数值合同 |
| `logger_rotation_config_t` | 公开配置的内嵌对象及默认初始化器 | 结构布局、字段偏移 |
| `logger_state_t` | get_state 的输出类型与诊断状态 | 函数类型、状态常量 |
| `logger_source_t` | source 记录接口按值传递的对象 | 函数类型、C/C++ 布局／初始化器 |
| `logger_overflow_policy_t` | 公开 overflow 数组元素类型 | 配置布局、DROP/SYNC 数值合同 |
| `logger_metrics_t` | get_metrics 的公开输出对象 | 函数类型、结构布局 |
| `logger_io_metrics_t` | get_io_metrics 的公开输出对象 | 函数类型、结构布局 |
| `logger_diagnostic_flag_t` | 已公开的诊断标记类型名与常量 | 类型名消费、诊断位数值合同 |
| `logger_diagnostics_t` | get_diagnostics 的公开快照对象 | 函数类型、结构布局／字段偏移 |
| `logger_file_metrics_t` | get_file_metrics 的公开输出对象 | 函数类型、结构布局 |
| `logger_syslog_start_t` | syslog.startup 的公开枚举类型 | Syslog 配置布局、启动策略常量 |
| `logger_syslog_config_t` | 公开配置内嵌的 Syslog 设置 | 结构布局、默认初始化器 |
| `logger_syslog_metrics_t` | get_syslog_metrics 的公开输出对象 | 函数类型、结构布局 |
| `logger_config_t` | logger_create 的配置对象和既有初始化入口 | 函数类型、完整布局、安装消费 |
| `console_color_mode_t` | set_color 的参数和 console_config.color | 函数类型、配置布局／常量 |
| `console_verbosity_t` | set_verbosity 的参数和 console_config.verbosity | 函数类型、配置布局／常量 |
| `console_config_t` | console_init 的输入对象与默认初始化器 | 函数类型、结构布局、安装消费 |

上述布局／函数类型复用 `tests/packaging/current_abi_contract.c` 和安装消费入口；
单独的类型名消费也必须覆盖输出／诊断标记等未直接用作函数参数的公开名称。
不能仅以导出符号列表不变证明源码兼容或字段布局不变。

`src/logger_cleanup.h` 的九处类型定义承担清理类别协议，不是普通业务容器的隐藏别名：

| 定义点 | 保留的具体作用 |
|---|---|
| `DEFINE_CLASS` 的 `class_*_t` | 将传入的标量、指针或对象类型绑定到同名构造／析构对 |
| `EXTEND_CLASS_COND` 的 `class_*_ext_t` | 条件构造复用基础类别的精确析构参数类型 |
| `DEFINE_GUARD` 的 `lock_*_t` | 把生成的锁构造参数类型传给条件守卫扩展 |
| `DEFINE_GUARD` 的 `class_*_t` | 区分该类别的 resource/err 守卫对象和析构参数 |
| `DEFINE_GUARD_COND_4` 的 `class_*_ext_t` | 条件取得仍使用基础守卫对象及同一清理协议 |
| `lock_pthread_mutex_t` | 手写 pthread 守卫向上述条件扩展提供互斥锁指针类型 |
| `class_pthread_mutex_t` | 手写守卫承载 resource/err，并接入同一 CLASS/ACQUIRE 协议 |
| `DEFINE_LOCKDEP_MUTEX_GUARD` 的 `lock_*_t` | 将固定锁类别与具体 pthread 构造参数绑定 |
| `DEFINE_LOCKDEP_MUTEX_GUARD` 的 `class_*_t` | 将类别匹配的清理／显式 release 绑定到同一守卫对象 |

同一上游快照的 [cleanup.h](https://github.com/torvalds/linux/blob/c339f4010dea8b0d754f84ace70cd1b089878ef8/include/linux/cleanup.h)
也使用类别生成的 typedef 来配对构造／析构和条件扩展。这是对具体生成接口的依据，
不能推广成所有宏都可任意定义结构体别名。生成类型的编译正反例和资源清理回归继续必需；
此处只解释类型绑定，不代替宏求值、错误与锁协议的审查。
标量／指针 typedef 不创造新的 C 名义类型，也不能单凭类别名称提供独占所有权保证。

`tests/scripts/test_owned_fd_copies.py` 的 `owner_alias`、`owner_alias2` 和
`owner_pointer` 保留为故意构造的类型别名输入，用来证明既有 AST 规则仍可穿透别名链／指针。
它们不进入生产库，不能为排版或类型清理而删除这些测试目的。
`tests/bench_pr116_direct.c` 继续使用 #116 两个历史生产提交的 ABI；
它的专用对照构建不等于当前 ABI 2 消费，也不应为了当前头文件下的单文件语法检查改写历史接口。

## 注释

优先解释：

- 所有权；
- 生命周期；
- 锁机制；
- 阻塞上下文；
- 内存顺序；
- 持久性/崩溃不变量；
- 错误语义；
- 为什么替代方案不安全。

禁止只复述代码。

普通多行块注释的 `/*`/`*/` 各自独占一行，中间的 `*` 对齐；
单行说明可以保留在字段或简单声明旁。具体示例见[代码与注释规范](../CODING_STYLE.md)。
接口说明必须对应实际参数名，并区分状态、计数、谓词以及失败后的资源责任。
按组共享的合同无需为每个访问器重复；说明解除锁与证明对象存活的区别，
不能把拥有者槽失效、出队或失败回执直接写成物理释放、输出完成或回滚成功。

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

- 有界；
- 截断可观察；
- 格式字符串经过编译器检查；
- 长度计算经过溢出检查。

## 资源与错误

- 所有权转移使用带类型的 `take_*`；
- 词法作用域内的单一所有者资源可以使用清理辅助函数；
- 需承载错误的收尾、共享生命周期、线程生命周期保持显式；
- 不允许清理错误覆盖主错误；
- 资源大小、长度和容量等普通算术不能依赖无符号整数回绕来判断溢出或
  驱动正常控制流；运算前检查边界。
- 现有队列位置/槽位代次协议是限定例外：`src/logger_queue.c` 用
  `seq - pos` 的模差值判断槽位状态，构造时将容量限制在位置空间的
  一半以下。这不是资源大小溢出检查。该例外只按已有队列文档和回绕
  测试的范围验收，不表示任意长运行或任意暂停生产者后都已排除代次
  重合；改变序列类型、容量边界或复用规则时须重新审查。

## 并发

- `volatile` 不是同步、原子操作或内存屏障。
- 用户态并发必须首先满足 ISO C11 数据竞争规则。
- 原子操作、锁、引用计数、线程等待、生命周期固定不能互相替代。
- 无锁设计必须提供线性化、先发生关系（happens-before）、回收安全性证明。

<a id="review-原则"></a>

## 评审原则

2.0 代码优先“最小充分机制”。不能仅因为 Linux 内核存在某原语就引入；
必须先证明 c-logger 存在对应问题。

## 自动化门禁

针对进入 `main` 的 2.0 PR：

- `scripts/check_v2_diff.py` 只检查新增 C/C++ 行，避免为了启用规则而顺手重写 历史遗留代码；
- 新增代码禁止 `strcpy/strcat/sprintf`；
- 新增 `strncpy` 默认拒绝；确属固定宽度协议字段时必须在同一行标记 `V2_FIXED_WIDTH_OK` 并由评审者审核；
- 新增源码直接写 `__attribute__((always_inline))` 被拒绝，统一走编译器抽象层；
- `git diff --check` 负责空白字符错误；
- VLA 和大栈帧由编译器门禁检查。

这些门禁只约束新改动；清理历史技术债必须独立 PR，不能混入无关功能变更。

## 所有权命名

内部 API 的动词必须表达所有权/生命周期语义：

```text
take_*     转移所有权；成功后来源对象失效
get_*      获取独立生命周期引用
put_*      释放独立生命周期引用
borrow_*   获取不持有所有权的访问；不得延长生命周期
release_*  释放某项资源，但不一定结束容器生命周期
close_*    关闭文件描述符/后端等外部资源
destroy_*  结束对象生命周期
reset_*    复用仍然存活的对象
```

如果对象不是 REFCOUNTED，不应为了命名对称增加空洞的 `get_*/put_*`。

同一个动词不能同时表示普通读取和所有权转移。
