# 2.0 公开接口模糊测试与队列属性测试（#107）

范围：**当前没有 Audit 的普通 Logger 2.0**。原 #107 列出的旧 Audit/parser、
检查点、密码学记录解码、通用引用计数、RCU/SRCU、seqcount 不在当前产品里，
不能为填完历史清单虚构它们的语料与通过结果。

## 1. 两种相互独立的证据

| 检验 | 生产对象与判定原则 | 适用限制 |
|---|---|---|
| 配置/脱敏公开 API 模糊测试 | 与当前静态 Logger 生产库链接；同一 `LLVMFuzzerTestOneInput` 用于 Clang 真正的 libFuzzer、ASan/UBSan 和 Xmake 的固定种子伪随机属性回放 | 此阶段只约束受界的公开配置及脱敏输入，不会把任意字节强制转为无效指针或超出实际分配长度的 struct_size |
| 有界队列模型属性测试 | 实际 `logger_queue_push/try_pop` 对比独立 FIFO 参考模型；检查序列、无重复/丢失、容量、深度、空谓词、短/长消息精确正文、spill 归还及等待唤醒指标不受误触发 | 此测试为单线程的步骤生成，不替代现有八生产者 MPSC、队列唤醒 litmus、TSan 或 C11 memory-order 证明 |

### 公开配置模型

从 **已知安全、无需 I/O 和工作线程的同步 stderr 配置** 开始，
对 `version`、`struct_size`、未来零尾/非零尾、输出掩码、日志级别、
格式详细度、文件轮转枚举、权限、flush 级别与 DROP/SYNC 枚举分别变异。
失败码独立断言：`EPROTONOSUPPORT`、`EINVAL` 或 `E2BIG`；
合法配置成功后由同一结构性所有者调用销毁。
本模糊目标不将不受信任的字节直接用作文件路径、配置指针或
结构体长度；因此不是“所有 UAPI 无效内存访问都已测试”。

### 脱敏参考模型

生成长度不超过 96 的可打印 ASCII 秘密，变异 prefix、suffix、
输出容量与 NULL 值分支。参考模型**独立实现**遮挡决策：先裁剪
prefix/suffix；任何可能完整暴露秘密的组合都必须变成 `***`。
NULL 值使用 `[REDACTED]`。检查足够容量下的确切内容，
容量不足的 `ENOSPC`、零容量的 `EINVAL` 以及缓冲区前后哨兵，
不在本项目内部调用脱敏实现获取预期答案。

### 队列属性模型

每个固定种子生成 2500 次推入或弹出操作，队列容量遍历 2/4/8/16
并在边界附近反复填满、清空和复用槽位。正文长度涵盖
0、1、内联阈值的前/等于/后一字节及较长消息。
独立 FIFO 记录保存期望文本、长度与序列号；
每步核对真实队列深度/空谓词、spill 使用量不超过有效槽位，
最终必须完全排空且无残留 spill。
因为队列没有外部消费者等待，测试不擅自声称验证了通知路径；
已有 MPSC/唤醒专项回归继续承担该任务。

## 2. 重放、变异与发布门禁

`tests/fuzz/public_fuzz.c` 实现唯一的
`LLVMFuzzerTestOneInput` 和 `LOGGER_FUZZ_STANDALONE` 固定种子入口。

固定种子回放由 Xmake 核心测试目标 `public_fuzz_property_test`、
回归目标 `queue_model_properties` 执行；每轮打印确切 seed 和步数。
`tests/fuzz/corpus/` 中的九个纯文本初始语料适用于真实 Clang
libFuzzer。核心验证另编译**真实生产静态库**的 Clang
ASan/UBSan 版本，以 `-fsanitize=fuzzer,address,undefined` 加载
同一测试源。**测试专用** Clang 构建额外设置
`-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp`，并核对
`liblogger.a` 中确实包含 sanitizer 覆盖回调，否则失败关闭，
不能把仅 harness 插桩的覆盖数字冒充生产代码的引导结果。
这份插桩归档不是可安装或正式发布的生产二进制；
release-validation 继续单独检查未插桩的真实交付产物。
模糊任务限制 `-seed=107 -runs=3000 -max_len=128`，
并保存输入语料、SOURCE_COMMIT、库与驱动的哈希、覆盖回调符号
及原始 fuzzer 日志。

`public-fuzz` 在任务级统一设置 `ASAN_OPTIONS=detect_leaks=1` 和
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，使实际 Logger
模糊驱动与检测器自检使用同一运行时策略。
[Clang 的 UBSan 默认会在多数诊断后继续执行](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#usage)，
因此仅有日志和进程零退出不足以保证本轮未出现未定义行为。
每轮正式模糊运行前，`tests/regression/run_fuzz_sanitizer_exit.sh`
使用同一 Clang 与 `-fsanitize=fuzzer,address,undefined` 编译独立探针，
分别回放健康输入和刻意触发有符号整数溢出的输入。
健康输入必须零退出且有成功标记；溢出输入必须以 1 退出、包含
UBSan 的溢出诊断，并且没有继续执行标记。编译失败、超时或其他
运行错误均不能充当有效的错误检出结果。
探针源码、编译器、命令、输入、产物哈希、退出码及原始日志随
`public-fuzz-evidence/sanitizer-exit/` 一并留存，包括验收失败的运行。
该探针不链接 Logger；两次固定输入回放只验收检测器的退出策略，
独立于生产模糊驱动的 3000 次运行，也不等于当前 Logger 发现了溢出。

有限 `-runs` 和固定种子只是可复现的**有界持续集成烟测**，
不是长期覆盖率引导的模糊测试服务，更不能推导输入空间全覆盖。
若出现崩溃，必须保存触发的最小种子并转为稳定回归，
禁止直接丢弃失败样本或仅改变随机种子。
若生产源、接口或编译设置变化，全部证据以该精确 PR head 的
实际工作流结果为准，不复制历史成功作为当前审批依据。

## 3. 分层责任与后续范围

生产接口检查、队列对象/状态属性、线程并发 litmus、
ASan/UBSan/TSan、进程 SIGKILL 崩溃合同、打包/ELF/安装消费与
生产等价的固定工作负载基准独立运行、独立结项。
模糊输入的“没有崩溃”不意味着每条日志成功输出、设备数据
已物理持久化或出现了全部并发调度交错。

本规范确认有用的增量验证能力，不自动关闭 #107。
队列多生产者属性、格式化和配置更多边界、持续模糊测试覆盖趋势
仍可能有后续工作；正式 v2.0.0 标签/Release 必须另行授权。
