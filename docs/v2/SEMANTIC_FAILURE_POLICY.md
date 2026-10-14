# 生产资源与同步语义检查及故障覆盖（#106）

本文件给出现行普通 Logger 2.0 的可执行语义门禁、实际故障注入来源和
不适用项。**静态检查、模拟失败、Sanitizer 与生命周期证明不能互相替代。**

## 1. 当前静态检查的分工

| 门禁 | 直接验证的边界 | 实现与测试 |
|---|---|---|
| 编译器类型/大小基础约束 | GNU C11、错误返回类型、禁止生产 VLA、大栈帧检查，当前公开 ABI 精确布局与函数类型 | Xmake 生产编译，C11/C++11 ABI 快照 |
| checked mutex 与锁跟踪 | 禁止丢弃若干互斥锁结果；实例 guard 必须保存获取错误、显式检查解锁；锁类实际运行前检测 | scripts/check_lock_error_policy.py、lockdep_regression 正反用例 |
| C11 CAS 内存序 | 显式成功/失败内存序、合法组合、未经评审的修改和新增 CAS 拒绝 | scripts/check_cas_orders.py、配置清单及负向变异 |
| 新增源码行与格式 | 特定不安全字符串函数、禁止直接 always_inline、源码差异格式及新增行 | scripts/check_v2_diff.py、check_source_format.py |
| **资源/同步失败语义** | 对 src/、include/ 的指定 pthread rwlock/cond 等调用结果丢弃和 close(EINTR) 循环重试失败关闭 | scripts/check_semantic_failure_paths.py、tests/scripts/test_semantic_failure_paths.py |

新的资源/同步语义检查器**扫描整个当前生产源码树，而非只检查新增行**；
由拉取请求门禁和 Ubuntu 20.04 GCC/Clang 工程基线同时执行。
正例包含合法的 fsync(EINTR) 重试、检查过的 rwlock/cond 结果、
注释与字符串中的示例；负例包括 do/while 和 while 形式的 close 重试、
(void) 丢弃以及裸 pthread 条件等待/读写锁调用。

Linux close(fd) 受 EINTR 中断时，文件描述符可能已经由内核释放，
并被其他线程重用；因此不能再对同一个整数描述符调用 close。
关闭错误应报告/记录，已消费的 fd 不能因此重新交给新的关闭循环。
这条规则**不能**机械用于 read/write/fsync 的 EINTR 重试。

词法检查器的定义范围是可识别的独立调用表达式与简单循环。
复杂宏、跨函数数据流、别名、间接函数指针、伪造的同步语义均超出能力。
它不是 C 解析器、Sparse、Coccinelle 的完全替代，也不能声称所有
unchecked pthread 或 ownership 问题已经穷举。涉及新资源所有权的 PR
仍须人工说明谁创建、所有者、转移点、失败回滚和最终释放。
现有 mutex guard 的特殊 cleanup destructor 按既有单独检查器处理，
不以统一禁止所有 (void) cleanup 调用来制造误报。

## 2. 失败注入和正反回归映射

| 资源/协议边界 | 真实故障测试入口（当前源码） | 判定内容与限制 |
|---|---|---|
| 候选 malloc/calloc、队列和同步对象构造 | test_constructor_rollback.c、test_destroy_status.c | init/destroy 失败、部分构造和最终 free 计数；不是任意分配点的形式化覆盖 |
| pthread create/join、worker 抢先失败 | run_worker_startup.sh、run_worker_failure.sh | 真实生产静态库的启动和工作线程故障、join/停止/保守保留；注入的 EIO 是受控模拟 |
| 互斥锁/读写锁、条件等待/通知与锁类 | test_lock_failure.c、test_lockdep.c、test_global_lifecycle.c | 获取错误不进入临界区、解锁未知保留、条件错误通知、递归/ABBA 与 R7 对象阶段；不证明所有 pthread 锁天然会返回模拟错误 |
| Global 对象发布和退役 | run_global_r7.sh 与 global_r7_publish_cas_poison | 生产静态库阶段与真实 final free；精确 RUNNING CAS 失败由同源码 LOCKDEP=1 测试归档注入，**不冒充生产 .a 内部直接暂停** |
| 文件 openat、写入/短写、fsync、close、rename 与轮转 | test_file_backend.c、test_destroy_status.c、run_close_eintr.sh、test_rotation_mutations.py | 真实生产库在 close 后合成 EIO/EINTR，断言同一 fd 只关闭一次、容器最终释放；轮转状态错误可见，但不证明设备物理掉电 |
| Syslog socket/connect/send/close、EINTR 和背压 | test_syslog_faults.c、test_syslog_config.c | 非阻塞重连、容错和计数；不证明 Syslog 守护进程已持久化 |
| bounded MPSC、完成水位、队列等待 | test_queue_wakeup_litmus.c、test_worker_failure.c | 发布、唤醒、预留空洞、已进入 flush 的失败退出与 DROP/SYNC；不代替 C11 全协议推导 |
| 生产支持库隔离 | check_production_artifact.py、生产/私有支持库对照和发布包检查 | 生产 LOCKDEP=0、FAULT_INJECTION=0，故障环境变量/符号不得进生产 ELF/归档与安装包 |

故障注入不是新的公开 API：极少数必要的旧故障宏仅存在于不安装的
logger_test_support；多数复杂故障通过测试可执行文件中的 GNU ld
链接期包装器实施，正常生产库不承担插桩分支或动态测试开关。
新的 malloc、文件描述符、线程或锁获取点需要随对应 PR 增加实际
失败/清理测试或明确说明已有覆盖，不允许把成功路径通过误称回滚已验收。

## 3. 不适用项及剩余范围

- Audit 持久编码/密码学/恢复、熵源与安全密钥管理不属于现行 ABI 2
  普通日志产品；历史问题 #105 是**不再计划**而不是其失败矩阵已通过。
- 当前没有公开 refcount_t、RCU/SRCU 或 seqcount 所有者，因此不伪造
  这些不存在原语的类型检查、终止状态测试及 fault hooks。
- 生产代码的类型基础规则采用编译器诊断、公开 ABI 断言和
  [所有权模型](OWNERSHIP.md)审查；没有声称新增独立的完整类型系统、
  对所有整数引入强类型或完成了所有 AST 级语义检查。
- 测试归档是模拟输入控制点，不承担模拟物理掉电、NFS/SMB、
  不可控 I/O 阻塞或跨任意 libc/内核的时间保证。
- 所有工具输出必须归属具体 GitHub PR head、工具链和运行记录。
  本文只是门禁规则与矩阵，不能作为没有实际运行就通过的证明。

对应执行方式：运行 Python3 标准库单元测试、静态扫描，再执行项目
原有的 Xmake 完整测试、ASan/UBSan、TSan、真实安装消费和平台门禁。
静态扫描失败须修改危险控制流并增加负向场景，不能通过放宽规则直接忽略。
