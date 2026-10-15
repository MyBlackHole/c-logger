<a id="testing"></a>

# 测试

> main 的源码、生产库和未安装支持库均为不含 Audit 的 ABI 2。普通 Logger 的生产链接、故障、锁、
> 格式、文件崩溃、安装消费与制品隔离独立验收。旧 v1 快照不再是当前门禁。

Xmake 是项目唯一的构建与测试权威。所有测试都由 `xmake.lua` 注册，CI 与本地开发使用同一
目标/测试定义。下游安装包仍使用真实 CMake 消费方验证 `find_package(Logger ...)`，
但项目自身不再依赖 CTest。

## 基本约束

运行 Xmake 前先导出唯一版本源：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
```

默认测试构建使用 Linux/ELF、GNU C11、`-Wall -Wextra -Wpedantic -Werror`。生产产物、
故障/崩溃测试和白盒回归分离：

- `logger`：真实生产产物，故障注入关闭；
- `logger_test_support`：仅测试使用，包含 `src/logger_fault.c`；
- `logger_regression_support`：同源白盒静态库，用于 GNU ld `--wrap`
  与内部确定性回归。

后两者永不安装，也不进入 XPack。

## 本地入口

<a id="fast"></a>

### 快速测试

仅运行直接链接生产库 `logger` 的核心测试：

```sh
scripts/check.sh fast
```

等价于：

```sh
xmake f -m release -o build \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=n \
  --build_regression_tests=n
xmake test -j4
```

### 生产构建

运行当前静态库/完整测试面，包括核心测试、私有故障/崩溃支持与全部回归测试：

```sh
scripts/check.sh production
```

完整测试集默认串行执行。部分历史测试共享固定文件名或工作目录，而测试内部的真实并发、
取消、fork 和压力测试行为不受串行运行器影响。

### 专项崩溃测试

8 个普通 Logger 持久性进程崩溃用例使用 Xmake 原生 `process-crash` 分组：

```sh
scripts/check.sh crash
# 或
xmake test -g process-crash -j1
xmake test -g crash-verifier -j1
```

固定集合：已确认基线、普通数据 fsync 前/后、真实轮转后的数据 fsync 后、以及
四个现有文件轮转切换中断点。每项都在管道确认真实中断点后由父进程 SIGKILL，
枚举当前文件与归档文件逐字节验证唯一基线，再重开、追加和验证两个恢复记录。
fsync 前的未确认尾部与已确认记录分开验收，普通 Logger 不自动修复残缺尾部。

原 9 项中的三个纯 Audit 检查点明确退役；普通 Logger 没有对应检查点契约。
详细新旧映射、未确认尾部规则和真实切点见 [普通 Logger 崩溃契约](docs/LOGGER_CRASH_CONTRACT.md)。
专用 `crash-recovery` CI 在 main 推送/手动运行时对共享/静态配置各执行 3 次，PR 执行共享配置一次；
私有崩溃测试程序始终链接静态测试支持库，不代表生产共享 DSO 注入。
`scripts/check_crash_matrix.py` 核对精确八项并输出 JSON、JUnit 和原始日志。
`crash-verifier` 用正负向变异测试样本验证验收器不会放过缺失、重复或损坏。

<a id="rotation-记录完整性与反例"></a>

### 轮转记录完整性与反例

`rotation_test` 每次使用独立 `mkdtemp` 目录，检查关闭操作的返回状态，要求产生归档文件，
并按归档文件名的时间顺序加上活动文件尾部，逐条验证 100 条完整记录连续且恰好出现一次。
`xmake test -g rotation-verifier` 将同一测试链接到测试专用替代实现：有效测试样本必须通过；
无输出、仅有活动文件、空归档、缺失、重复、内容损坏、记录/文件倒序与关闭失败必须拒绝。
每项分别在干净目录和预放旧活动/归档文件的目录执行，旧文件不得影响结果或被删除。
替代实现不链接生产库、不安装，也不进入 XPack；真实轮转测试仍分别链接静态/共享生产产物。

<a id="raw-fork-防御"></a>

### 原始 fork 防御

生产链接的 `fork_test` / `fork_guard_test` 与白盒 `process_fork_regression` 持续验证
PID/atfork 防护、注册窗口、早注册的子进程处理函数、继承锁和 ECHILD 快速拒绝。
`process_fork_regression` 在共享配置下仍链接同源静态支持库；生产核心测试分别覆盖真实共享/静态产物。旧 `legacy-fork` 配置与重初始化辅助接口已移除。

<a id="旧-profile-参数"></a>

## 旧测试配置参数

历史 `scripts/check.sh unit/integration/concurrency/reliability/security/host-owned`
依赖 CTest 多标签。Xmake 的测试分组是单组语义，无法无损表达同一用例同时属于多个标签。

为避免旧命令变成“看起来成功但实际少跑”的兼容陷阱，这些参数当前仍接受，但会运行**完整
Xmake 测试集的保守超集**。后续只有在真实开发工作流需要时，才新增 Xmake 原生专项分组；
不会为了复刻旧标签体系重新维护第二份测试分类表。

## Sanitizer 检测

CI 对完整默认静态测试集运行两组 Sanitizer：

ASan + UBSan：

```sh
xmake f -m debug -o build-asan \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y \
  --policies=build.sanitizer.address,build.sanitizer.undefined
xmake test -j1
```

TSan：

```sh
xmake f -m debug -o build-tsan \
  --build_shared=n \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y \
  --policies=build.sanitizer.thread
xmake test -j1
```

Sanitizer 后仍对生成的生产库 `liblogger.a` 运行
`scripts/check_production_artifact.py`，确保测试钩子没有进入生产产物。

## 共享库集成

共享库配置除生产库链接核心测试外还覆盖真实 DSO 边界，包括：

- 共享库 Syslog 后端；
- 全局共享库压力测试；
- 当前 C/C++11 公开 ABI 消费方；
- `dlopen/dlclose` 生命周期；
- 安装包消费方。

需要完整共享库测试集：

```sh
xmake f -m release -o build-shared-tests \
  --build_shared=y \
  --build_tests=y \
  --build_private_tests=y \
  --build_regression_tests=y
xmake test -j1
```

白盒 `--wrap` 回归测试即使在共享库配置中也链接同源静态
`logger_regression_support`；只有需要验证真实 DSO 观察面的用例才直接链接生产共享库。不能把静态包装测试误称为 DSO 内部拦截。

## 故障测试范围

当前维护普通 Logger 的八个进程 SIGKILL 切断点与判定器变异测试。
VM/断电测试及其专用程序、运行脚本和 CI 已移除，不再提供可选执行入口。
历史报告只描述对应旧提交，不是当前支持承诺。进程测试保留宿主页缓存，不能证明
物理掉电或设备易失缓存安全；既有 flush/fsync API 契约不变。
最低内核 VM 属于平台兼容性验证，继续独立保留。

<a id="queue-基准测试"></a>

## 队列基准测试

当前候选版本的 `bench_matrix` 由 Xmake 构建，直接链接真实生产静态 `logger`，
release 下同时优化库和驱动；无需启用私有或回归测试。私有队列观测接口不导出到
共享 DSO，因此共享配置不提供此目标。公开 API 的 `bench_logger` 支持两种配置。
具体运行命令与测量边界见源码树中的 `BENCHMARK.md`（不随生产包安装）。

原 `benchmark_accounting_smoke` 转移到 `bench_matrix_accounting_regression`，
继续在静态/共享回归配置中使用原支持库和同一组计数断言，不作为性能测量。
静态 `build_tests` 配置另注册生产链接的 `production_benchmark_accounting_smoke`。
`python3 tests/scripts/test_benchmark_build.py` 在临时源码副本实际构建并检查优化、
宏、链接命令和 ELF 依赖，运行小样本及两个配置的回归 smoke；还检查共享配置拒绝
私有基准。`xmake-parity` 的 `aux-targets` 执行此检查并上传构建证据。

两个冻结历史基线提交继续用各自
提交中原有的 CMake 构建，以保证历史基准可重现，而不是用今天的构建描述重解释过去数据。

`queue-benchmark` CI 保持原线程数、记录大小、重复次数、内存缩减阈值与比较
脚本的确定性内存门槛不变。吞吐只作观察；历史构建与当前生产配置未证明等价，
不能把比较比值当作单项源码优化收益或生产性能达标证明。

## 安装与发布包验证

发布验证对共享库/静态库两种 XPack 都执行：

1. 完整 Xmake 测试集；
2. 生产产物隔离；
3. 共享库 ELF ABI / SONAME / 符号版本检查；
4. XPack TGZ + SHA-256；
5. 解包后的路径迁移；
6. 独立 C/C++11 CMake 消费方；
7. PIC SDK 模块；
8. pkg-config 消费方；
9. 精确版本/组件失败关闭；
10. 当前公开布局、默认值、函数类型及配置负向契约；
11. 显式安装文档清单、`docs/v2/` 权威与全部 Markdown 相对文件链接的重定位验证。

`tests/packaging/test_installed_docs.py` 包含缺失 v2 文档、缺失传递链接、越界链接、
意外安装历史 Audit 文档等负向测试样本；发布检查通过 `check_install.py` 使用同一验收器。

`examples/installed_consumer/CMakeLists.txt` 和发布包中的 `LoggerConfig.cmake` 是**消费兼容性**
测试资产，不代表项目重新依赖 CMake 构建。

## CAS 内存序门禁

启用 `build_tests` 时，`xmake test -g cas-order -j1` 注册并运行静态清单检查和负向变异测试。
`validation/cas_orders.tsv` 记录 `src/` 与 `include/` 中每个直接 CAS 的对象、目标值表达式、
强/弱类型、成功/失败内存序、数量和理由，不依赖行号。新增、删除或改变 CAS 时须同步
评审清单，不能自动接受扫描结果。检查器拒绝不在保守 C11 允许表中的内存序组合（包括
release/acquire）、隐式内存序、内存序别名和未经评审的编译器 CAS 内建操作。

变异测试在临时源码副本中逐处恢复四个全局生命周期 CAS 的 release/acquire，确认门禁失败；
也验证非法失败内存序、合法但未评审的内存序变化、清单漂移和新增调用会失败。
这是未经预处理的词法门禁，检查所有源码分支，不是完整 C 解析器、先发生关系
（happens-before）证明或运行时未定义行为检测；生命周期故障/并发回归仍须独立运行。
保守内存序组合消除 C11 可移植性契约疑义，不宣称旧组合已复现未定义行为。

<a id="工程-gate"></a>

## 工程门禁

测试通过不是唯一准入条件。较大的队列、工作线程、后端、生命周期、所有权、锁机制或
公开 API 修改还必须遵守下文列出的工程约定。

<a id="observability-regression"></a>

## 可观测性回归

统一诊断数据通过现有真实故障场景验证，而不是单独伪造计数：

- 队列已满：检查当前深度/容量、完成积压与饱和状态；
- DROP / SYNC 回退：检查独立生命周期观测标记；
- 长消息溢出存储耗尽：检查使用量/容量/耗尽次数；
- 后端 write/fsync 失败：检查 failed_records 与粘滞首错 first_error；
- 文件输出端：检查 write/writev 系统调用、部分字节、数据/目录 fsync、轮转、重新打开分类计数；
- 刷新后：队列深度、溢出存储使用量、完成积压回到 0；
- 全局代次：诊断数据与指标一样受代次固定/过期代次准入门禁保护；
- 显式重入：同线程嵌套读取被 EDEADLK 拒绝并清零输出；
- 安装包消费方：使用安装后的公开头文件/库实际调用诊断接口。

公开 API 修改还必须遵守：

- `docs/ARCHITECTURE_INVARIANTS.md`
- `docs/RESOURCE_OWNERSHIP.md`
- `docs/REFCOUNTING.md`
- `docs/RESOURCE_CLEANUP.md`
- `docs/LOCKING.md`
- `docs/LOCK_MATRIX.md`
- `docs/CONCURRENCY.md`
- `docs/ERROR_HANDLING.md`
- `docs/LIFECYCLE.md`

新增资源获取/转移路径应有对应失败覆盖；测试通过不能替代所有权、锁顺序和最终释放
关系的可证明性。

<a id="cancellation-边界"></a>

## 取消边界

调用方处于 `PTHREAD_CANCEL_ASYNCHRONOUS + PTHREAD_CANCEL_ENABLE` 时进入 Logger/Console
不在支持契约内。支持边界是延迟取消，或调用前已禁用取消。
取消策略恢复回归测试会验证原先禁用取消的调用方仍保持禁用状态，且取消类型
不被破坏；构造函数契约会拒绝启用的异步取消。


## 普通 Logger 格式与隔离

静态生产目标 `logger_format_production_test` 保留 normal、cache-hit、localtime、date、
zone、bad-date、bad-zone、cached-bad、truncation 九项；`format_checked_production_test`
保留容量边界、严格/普通格式一致、printf 失败与非法纳秒断言。独立入口
`tests/regression/run_logger_format.sh BUILD_DIR [address,undefined]` 同样只链接真实生产静态库。

`production_isolation_regression` 直接链接当前静态/共享生产库，
`test_hooks_regression` 链接未安装的故障支持静态库。两者对照 file_write/file_fsync/
file_rename/short-write/syslog-path 与四个真实轮转环境钩子。process_fork 保留17个普通
Logger/Console/context场景；原21项的4个Audit入口专属场景退役。锁失败保留6个普通场景。
逐项迁移记录见 [Audit 清理覆盖映射](validation/AUDIT_TEST_RETIREMENT.md)。

## 固定到达率基准的验收器

`bench_fixed_rate` 使用真实生产公开 API，静态/共享都可构建。运行和证据边界见
`BENCHMARK.md` 的“固定到达率与真实文件”章节。`tests/scripts/test_fixed_rate_benchmark.py`
独立验证原始数组分位、缺失/重复/损坏文件与未尝试记录的拒绝；这类验证器单测不是性能验收。

## 两轮工程复审的验证证据与失败路径矩阵（#130）

此矩阵记录**当前不含 Audit 的 ABI 2** 对原始复审 #121 已确认问题的
测试入口与验证边界，不把规则扫描、编译成功、故障注入或 CI 整体绿色
互相替代。原始问题复现固定在历史源码 06efa9e139e0d589c77d42456f8a62e7925cc795；
对应修改均保留自己的 PR head 和 CI 证据，历史结果不能冒充最新 main 重跑。

| 契约 | 修复/历史范围 | 真实可证伪验证 | 仍不证明什么 |
|---|---|---|---|
| #122 审计编码拒绝非法时间 | #131，旧含 Audit 代码 | 历史 #131 的正常/格式错误生产库对照及原始日志 | 当前 ABI 2 已移除 Audit/密码学；不能重引历史专用测试作为当前发布前提 |
| #123 构造与工作线程抢先失败 | #132 | core-validation 运行 run_worker_startup.sh：normal、sync、create-error、worker-first、worker-live、join-error、destroy-error、creator-first、global-worker-first；直接链接实际生产静态库并检查 join/free、errno | 不能自动证明运行中 flush/watermark |
| #124 工作线程失败与 flush 终止 | 已修复 | core-validation 运行 run_worker_failure.sh：等待者先/后、预留空洞、multiple、spurious、取消、后端错误与正常 drain 共 13 场景；生产库链接期注入与 ASan/UBSan、TSan 对照 | 人为合成 EIO 不等于默认 pthread_cond_wait 常见失败；故障成功退出不能冒充任意环境活性证明 |
| #125 锁获取/释放错误传播 | 已修复 | xmake 中 lock_failure_regression：normal、emit、emit-unlock、queue-notify、queue-signal、force-wake 六项；实际 pthread 包装入口注入后断言未错误执行临界区/解锁及错误可见性 | 不说明有效非健壮 mutex 正常会返回合成的 EAGAIN |
| #126 锁序在真实阻塞前检查 | 已修复 | lockdep_regression 的 real-recursive、real-abba、real-unlock 和 trylock/rwlock 正反例；错误路径在子进程中预期中止，检查不会等到死锁后才触发 | 跟踪六类已建模锁，不是完整进程中所有 pthread 锁的自动证明 |
| #127 双 SC 栅栏最小模型 | #195 | N1570 7.17.3(11) 双全序推导及 RMW 完整协议边界 | 最小模型成立不等于旧整个 MPSC 已被证明 |
| #128 唤醒改动性能成本 | #197 | 两次历史 #116 生产静态库同条件直接前后对照，完整逐轮计数、延迟分位、DROP/回退与二进制 SHA；详见 validation/PR116_DIRECT_PERFORMANCE_20261012.md | 历史 ABI 1 无法代替当前 ABI 2 的硬件性能/业务 SLO |
| #129 销毁前静默期与安全释放 | #196 | destroy_status_regression 与 constructor_rollback_regression：正常/失败返回、外部 borrow 责任、worker join、free 拦截及资源保留 | pthread destroy 的成功返回不是外部无使用者证明 |

### 配置一致性硬门禁

现行 Xmake 构建边界是：

- 生产 logger：LOGGER_ENABLE_LOCKDEP=0、LOGGER_ENABLE_FAULT_INJECTION=0。
- 回归支持库 logger_regression_support：LOCKDEP=1、FAULT_INJECTION=0。
- 私有故障支持库 logger_test_support：LOCKDEP=1、FAULT_INJECTION=1，绝不安装。
- lockdep_regression 翻译单元自身必须 LOCKDEP=1，以防在测试源中意外
  编译为无操作内联；实际真实锁用例必须按预期中止，而不是只调用跟踪器。

除了既有 Xmake 实际编译、负向回归和生产制品隔离，xmake-parity 的 regression-tests
在成功运行回归后，直接对同一个 tests/regression/test_lockdep.c
执行正反编译：LOCKDEP=1 必须成功、LOCKDEP=0 必须因专用编译断言失败，
并核对实际失败消息。**这不是文本搜索替代语义**；生产库/回归库本身是否正确链接
仍由 Xmake 实际编译与测试结果检验，不因扫描宏字符串绿色就认为并发安全。

### 证据身份和阶段状态

- 实际 GitHub CI 的 SHA、workflow/job/run、工具链、构建产物、原始错误输出，
  应引用相应 PR 的精确 head；例如 #132 的 [完整验证记录](https://github.com/MyBlackHole/c-logger/pull/132#issuecomment-6037796635)
  与 #196 的 11 项工作流，不能替代未经执行的新提交身份。
- 某些链接期测试产物的 SHA 记录在其 CI 日志和运行制品；若没有产物哈希或
  此轮未重新执行，必须明确写“本轮未运行”，不能复制旧 SHA 假装新测试。
- #103 的设计文档、锁矩阵、原子交接及用户态可行性结论已经落地；
  泛化原子 load/store 包装、generic completion、seqcount、RCU/SRCU
  在当前生产设计中均有理由不采用。是否完成**全部**并发/对象阶段仍需
  分别检查实际失败矩阵、边界、R7 控制器阶段前提，而不能靠旧总议题勾选或 PR 数量。
- #121/#199 的 R7（控制器持有尚未发布的候选/退役对象时的实例锁阶段前提）
  已由 #200/#203 分别完成生产库场景和精确 CAS 白盒验收；
  类别跟踪器仍不能单独证明对象阶段，也不证明任意调度交错。
- 纯风格债务只做独立提交，不与上述生命周期、并发、锁语义问题混在一起。

本节是验证入口、已知证据和未验证边界的索引；不自动扩大正式 2.0.0
发布硬门槛，不恢复已经移除的 Audit/VM 断电测试，也不虚构未指定的生产 QPS/P99 阈值。

## Global R7 候选/退役对象的生产库验证（#199）

对象阶段不等同于锁类别。当前生产态的完整证明和限制见
[Global R7 阶段证明](docs/v2/GLOBAL_R7_PROOF.md)。

专项入口使用真实生产静态归档，正常模式与 Sanitizer 可共享测试源：

    bash tests/regression/run_global_r7.sh core-build
    bash tests/regression/run_global_r7.sh sanitizer-build address,undefined
    bash tests/regression/run_global_r7.sh sanitizer-build thread

每个注入场景为独立进程，覆盖候选发布失败后实际 free、销毁没有回执的
retained 对象、STOPPING 回滚、写锁解锁失败、最终释放后的非致命错误与
旧读者的完成衔接。驱动捕获实际 logger_t 地址并拦截 free 校验，
不从 pthread destroy 返回值或 live_objects 归零推导最终 free。
记录源码完整 SHA、归档/驱动 hash、编译器、各场景退出结果。
#200/#203 已分别完成相应阶段的实际验收；新源码运行仍须记录本次身份，
不能复制历史通过充当新测试结果。

## #104 对象设计验收入口

[2.0 对象与状态机设计](docs/v2/ARCHITECTURE.md)集中记录已实现的
显式 Logger、Global、MPSC worker、文件、Syslog 和 Console 决策；
[资源与阶段不变量](docs/v2/INVARIANTS.md)列出不得由局部优化破坏的条件。

现有验证在不同目标中分别承担不同责任：工作线程启动和失败、Global
发布/代次/取消、真实 R7 生产库最终释放、队列唤醒与完成谓词、后端故障、
公开 ABI 和安装消费方均使用各自的实际可执行回归。只有源码状态机论证
不能证明运行时正确；只有 CI 绿色也不能证明所有未注入的并发交错。

#199 对 G_STARTING→G_RUNNING CAS 失败的精确窗口另注册
global_r7_publish_cas_poison：在 LOCKDEP=1 的回归支持库中对
真实 logger_lockdep_assert_held 与 pthread 解锁入口实施同步交错。
它依赖同源的测试专用断言，不属于普通生产归档的精准注入。
已有真实生产库常规/故障测试与该白盒用例需要各自运行、分别报告来源；
不得因脚本命中相邻状态就声称所有可能调度交错都已证明。
#104 已按架构设计阶段验收，#108 已按公开错误模型与可执行 ABI 合同阶段
验收；这些阶段完成不代替最终固定候选验证，正式发布仍依赖独立标签授权。

## #108 公开错误模型生产测试

在 build_tests=y 的真实生产链接核心测试中，api_contract_test
使用同一头文件与实际生产库验证公开错误约定：

- 创建 NULL 配置须返回 NULL/EINVAL。
- 显式 flush/reopen 的 NULL 实例须返回 -1/EINVAL。
- 历史同步状态记录入口必须保持 -EINVAL，而非通用 -1+errno。
- 未初始化 Global flush 为 -1/ENODEV；无效 Global 创建为 -1/EINVAL。
- NULL destroy 成功并保留调用方原 errno。
- 正常同步实例创建、flush、destroy 可以完成。

配置版本与未来尾部检查由 config_abi_test 独立覆盖；C11/C++11 ABI 布局、
47 符号、SONAME/符号版本和安装消费由发布工作流独立覆盖。最终接口冻结
必须以批准的固定提交重新审核，不得将设计选择或旧 CI 结果冒充发布通过。

## #106 构造资源逐点故障注入

`tests/regression/test_ctor_resource_failure.c` 在测试可执行文件中以
GNU ld --wrap 拦截真实生产静态库对 calloc、malloc、pthread_mutex_init、
pthread_cond_init 的调用。正常路径先验证当前的 9 / 1 / 3 / 2 次获取，
随后由每个独立进程模拟 1 次获取失败，并确认：

- 构造返回 NULL 并保留 ENOMEM / EAGAIN 的原始失败码；
- 未启动 worker、Logger 的 live_objects 归零；
- 每个成功取得的堆分配均被真实 free 一次，不以析构返回码冒充 free；
- 清除故障后同一进程能重新创建、刷新并正常销毁一个异步实例。

测试目标通过 Xmake `ctor_resource_failure_regression` 注册；
`run_ctor_resource_failure.sh` 额外直接链接**当前检出的生产静态归档**，
记录源码 commit、库/驱动 SHA-256、编译器和每个失败场景的原始结果。
在普通、ASan+UBSan、TSan 配置下运行，且不启用生产故障宏或增加
运行时代码。只有精确 PR head 的 CI 成功，才能将对应清单条目
由 adjacent 正式判定为 direct。

#206 已验收构造阶段的 **18/22**；#207 已完成四个文件目录/协调锁/DIR
获取点的生产库精确故障验收并合并，当前清单为 **22/22 direct**。
其中每项 direct 表示已审查的指定返回错误路径，不是所有故障模式的穷尽；
新获取点或获取表达式变化必须重新提供实际证据。

## #106 文件资源取得失败的逐点回归

`tests/regression/test_file_acquisition.c` 通过 GNU ld `--wrap`
对真正的生产静态库四种入口进行精确失败控制：

- bind-directory：`open(O_DIRECTORY)` 返回 EACCES，禁止错误发布，
  未获得的 fd 不可伪造为需关闭资源。
- lock-file：`openat(... .logger.lock ...)` 返回 EMFILE，
  已取得的父目录 fd 必须归还，后继初始化能够成功。
- retention-directory：轮转成功、进入保留清理时，
  `openat(dirfd, ".", O_DIRECTORY)` 返回 EMFILE，
  不得提前关闭新活动 fd；可在后继轮转恢复。
- fdopendir：轮转后获取到临时目录 fd，但 `fdopendir(fd)`
  返回 ENOMEM；该调用没有接管 fd，其原所有者必须实际关闭一次，
  不能产生已释放 fd 的复用或双重关闭。

附加 healthy 正常对照，合计 5 个独立进程场景；
测试会统计由生产文件后端获得的 fd 与关闭/closedir，
核对系统进程 fd 数恢复、已取得描述符释放、新活动文件不因
单次保留清理失败错误退役。原有错误响应与恢复边界分别断言。

`tests/regression/run_file_acquisition.sh` 直接链接**当前检出的生产
liblogger.a**，而不是支持库，并输出精确 SOURCE_COMMIT、
编译器、被测库和驱动 SHA-256 及每个场景结果；普通、ASan+UBSan、
TSan 都纳入 core-validation。Xmake 回归目标另外共享同一份测试源。

[#207 核心验证](https://github.com/MyBlackHole/c-logger/actions/runs/38552614753)
已经通过，最终合并为 `2dfd9fef7c3d95cd36409c5a559da1edf90492fe`。
该历史证据不能冒充后续源码的新运行，也不能替代所有系统调用返回错误、
极端文件系统语义或物理掉电证明。

## #106 资源获取审查清单

`validation/resource_acquisitions.tsv` 在本次基线列出全部 22 个直接
堆分配/描述符获取/线程创建/同步对象初始化位置，各条记录带获取参数、
所有者、现有测试文件、验证等级（direct 或 adjacent）及理由。
`adjacent` **不表示**该位置已单独被注入失败；这是后续
设计审核和新增真实故障测试的清单，不是清除所有资源回滚风险的证明。

    python3 tests/scripts/test_resource_acquisitions.py -v
    python3 scripts/check_resource_acquisitions.py

检查器支持审查清单严格表头、字段非空、合法测试文件、重复项拒绝、
调用变化/新增/删除、参数常量变化和计数漂移的负向测试，并
保持注释/字符串、跨行规范化等合法源码不误报。
它与现有 CAS 内存序清单、锁返回值检查及运行时故障注入相互独立，
不引入生产代码与公开 ABI 变更。拉取请求与 Ubuntu 20.04 GCC/Clang
工程基线均执行上述命令；修改清单须人工复核真实所有权和失败回滚证据。

## #106 资源与同步语义门禁

[当前故障和语义门禁范围](docs/v2/SEMANTIC_FAILURE_POLICY.md)
明确区分完整编译器警告、mutex guard 检查、CAS 内存序清单、
新增行风格、资源/同步语义，以及真实生产库失败路径回归。
新增检查器不取代已有 `check_lock_error_policy.py` 和
`check_cas_orders.py`，只补充两条高风险规则：

- 生产源码里丢弃 pthread 读写锁和条件等待的返回结果时失败关闭；
- 生产源码里对 close() 的 EINTR 循环重试失败关闭；
  不拒绝按语义需要重试的 fsync/read/write。

链接期的 fd 故障回归也在 Xmake 的 destroy_status_regression 注册
close-error 与 close-eintr，并通过 run_close_eintr.sh 对本次源码编译
出的真实生产静态归档额外验证：先真正 close，再合成 EINTR/EIO，
要求只发生一次对该 fd 的关闭、报告错误、最终释放 logger_t。
该入口在普通/ASan+UBSan/TSan 中执行，不修改生产库或启用私有
测试故障宏，记录源码 SHA、编译器、归档与驱动哈希及逐场景成功结果。

在当前源码上直接执行：

    python3 tests/scripts/test_semantic_failure_paths.py -v
    python3 scripts/check_semantic_failure_paths.py

正反例覆盖合法 fsync 重试、注释/字符串、非法 close 重试、
被丢弃的同步操作以及源码目录缺失时的失败关闭。
拉取请求与 Ubuntu 20.04 GCC/Clang 基线均执行以上测试和实际检查。
当前版本没有 Audit/密钥熵源或通用引用计数原语，不生成伪覆盖；
任何新资源取得点仍要明确写出实际失败回滚测试、所有者及最终释放前提。

## #107 模糊测试与属性回归

[模糊测试与属性模型规范](docs/v2/FUZZ_PROPERTY_POLICY.md)明确公开
配置、脱敏和有界队列当前实际使用的变异范围、预期判定和未覆盖边界。
固定种子入口通过 Xmake 运行：

- `public_fuzz_property_test`：以 4 个固定种子各生成 2048 次
  受界公开 API 输入，验证实际 Logger 创建/错误返回及脱敏内容和哨兵。
- `queue_model_properties`：以 6 个固定种子各执行 2500 步
  入队/出队序列，与独立 FIFO 参考模型对照并验证溢出池归还。
- `tests/fuzz/corpus/` 包含 9 个可以直接用作 Clang libFuzzer 输入的
  原始文本种子。core-validation 的 `public-fuzz` 使用真正
  `-fsanitize=fuzzer,address,undefined`，并对当前 Logger 生产源码
  的**测试专用归档**启用 `inline-8bit-counters,pc-table,trace-cmp` 覆盖插桩，
  对归档本身存在覆盖回调进行失败关闭验证。正式交付库仍单独
  保持无模糊测试插桩；固定 seed 与有限运行次数的实际输入、
  源提交、产物哈希及回调符号均保留为证据。

这些测试故意不替代现行八生产者 MPSC、唤醒 litmus、锁错误注入和
进程 SIGKILL 崩溃矩阵。故障触发时必须保留并固化最小复现输入；
不能因为一轮 fuzz 无崩溃就把语义、性能或持久化宣布全部完成。
