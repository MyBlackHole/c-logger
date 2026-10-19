# #106：静态分析、语义门禁与故障注入逐项矩阵

当前事实先读[2026-10-18 发布收敛清单](RELEASE_CLOSEOUT_20261018.md)。
本表保留原 44 项映射及各阶段证据；#265–#272 的后续实现不由旧“需决定”措辞否定，
也不使尚未接受的完整要求自动结项。

> **2026-10-17 当前主线复核**：本表从历史PR #215的逐项审查移入最新主线，
> **不修改当时 E0–E9 历史证据的原始SHA、测试规模或阶段结论**。
> 当前审查基线为 `acb7a9978f530a88ca6787af9856237dcea85ce2`，
> 旧基线身份仍仅用于复核旧证据，不代表本次重新运行历史用例。
> 后来已经合并的 fork 运行域、公开ABI状态返回、XPack/安装消费和
> Linux 5.10 验收以 [发布前核查](../../docs/v2/RELEASE_PREFLIGHT.md)
> 与 [当前阶段增量](CURRENT_DELTA.md)为准。D1–D6的提议都没有维护者签收。


基线、E0–E9 证据身份及 D1–D6 决策状态见 [总索引](README.md)。
按原议题顺序逐项对应；“需决定”表示未满足原始完整要求，不是已经获得暂缓。

## 1. 类型与注解

| 条目 | 原始要求 | 当前事实/替代 | 分类、边界与下一动作 |
|---|---|---|---|
| T1 | 拥有/借用 fd | `src/logger_cleanup.h` 的 `take_fd` 失效来源，字段和接口仍使用普通 int；所有者见 E0/E1 清单 | 历史原结论：未有编译期隔离。之后PR #217已实现两处候选fd有限类型（未合并，不能算main完成）；仍需决定 D1 的其余范围，见 CURRENT_DELTA |
| T2 | 字节数/记录数 | 长度溢出、配置检查与槽位边界使用普通整数和显式检查 | 需决定 D1；不能因变量命名不同就称禁止单位混用 |
| T3 | 索引/计数 | `src/logger_queue.h` cap/mask/位置及 C 静态断言，E4 顺序模型检查容量与归还 | 需决定 D1；普通 size_t 不形成语义类型隔离 |
| T4 | 代次/序列 | Global 生命周期票据与 slot.seq 有不同协议，E8 推导及 E3/E4 回归 | 需决定 D1；协议正确性证据不等于编译器禁止混用 |
| T5 | 标志/枚举 | 公开枚举、掩码与 `tests/test_config_abi.c` 非法组合校验，E4 配置模型 | 需决定 D1；运行时值域检查已存在，完整静态类别约束未实现 |
| T6 | 状态 | `src/logger_internal.h` 状态与 E0 状态机、E8 发布协议 | 需决定 D1；C 枚举声明不自动禁止非法转换或状态转移 |
| T7 | 期限/时长 | 时间转换、超时错误和 E8 等待谓词；不是分别封装的期限/时长类型 | 需决定 D1；需逐个接口确认单位和溢出，不能用通用警告替代 |

`src/logger_compiler.h` 的返回值/格式/内联/静态断言抽象不是所有权或单位类型系统。
以上均保留原始“可由编译期或 lint 检查”要求；任何减少范围必须经 D1 接受。

## 2. 语义模式

| 条目 | 原始要求 | 现有入口与证据 | 分类及能力限制 |
|---|---|---|---|
| S1 | 忽略 pthread_mutex_lock 返回值 | `scripts/check_lock_error_policy.py`；`tests/regression/test_lock_failure.c`，E1/E3 | 已有证据：限定调用/guard 模式；不证明所有复杂数据流中的检查正确 |
| S2 | 忽略 logger_refcount_* 结果 | E0 当前对象没有该引用计数原语 | 不适用；不是该检查器已经实现 |
| S3 | close(EINTR) 错误重试 | `scripts/check_semantic_failure_paths.py`、`tests/scripts/test_semantic_failure_paths.py`、`tests/regression/run_close_eintr.sh`，E1/E2/E3 | 已有证据：简单 while/do 循环及真实 close 后合成 EINTR；不禁止合法 fsync/read/write 重试 |
| S4 | raw free 绕过 destroy | E0 所有权/静默期证明；`tests/regression/test_destroy_status.c` 拦截实际 free，E2/E3 | 需决定 D2；没有全面跨函数对象释放绕过检查 |
| S5 | raw 赋值绕过 take_* | 类型化清理/移交约定，`tests/regression/test_resource_cleanup.c`，E0/E3 | 需决定 D2；规范和测试不等于赋值流 lint |
| S6 | direct pthread_cond_wait 绕过等待抽象 | E0 明确对象专用等待谓词；E1 拒绝部分裸/丢弃返回值；`test_queue_wakeup_litmus.c`、`test_flush_pending.c`，E3/E8 | 通用 wait_event 包装不适用；现有等待错误检查已有证据；“所有直接调用是否合法”的完整语义检查需决定 D2 |
| S7 | 直接 publication atomic 访问 | `scripts/check_cas_orders.py`、`validation/cas_orders.tsv`、`tests/test_cas_orders.py`，E1/E8 | CAS 内存序登记已有证据；全部 load/store、别名及生命周期语义的自动审核需决定 D2 |
| S8 | 指定对象 malloc/free 绕过所有权 helper | `scripts/check_resource_acquisitions.py` 和 22 行清单；E2 指定获取失败 | 获取签名漂移已有证据；释放/逃逸/转移语义需决定 D2；不能称清单是完整 AST |

原始 Coccinelle/等价 AST 要求尚未完全实现。词法检查全树并不等于完整解析所有
预处理配置，更不等于跨函数所有权证明。现有门禁保留，不通过放宽规则获得绿色。

## 3. 风格门禁

| 条目 | 原始要求 | 现有机制 | 分类及边界 |
|---|---|---|---|
| C1 | 空白/缩进 | `scripts/check_source_format.py`、`.clang-format`、`git diff --check` | 已有证据 E1；改动范围检查不冒称所有遗留文件已重排 |
| C2 | 命名 | `docs/v2/CODING_STYLE.md` 的动词及所有权命名约定 | 需决定 D4；没有完备自动命名审查 |
| C3 | VLA | `xmake.lua` 生产 `-Wvla` 配合 `-Werror` | 已有证据 E1/E3；24KiB 栈帧警告另行限制，不是实际最大栈耗证明 |
| C4 | 禁用字符串 API | `scripts/check_v2_diff.py` 检查新增 C/C++ 行，固定宽度例外显式标记 | 已有证据 E1；不是全历史源码自动修复 |
| C5 | 未检查返回值 | 编译器属性、互斥锁和同步返回值专用扫描 | 限定模式已有证据 E1；全 API/全数据流的要求需决定 D2/D4 |
| C6 | FIXME/TODO | 编码审查约定 | 需决定 D4；未发现对应完整自动门禁，不将注释存在视为运行时缺陷 |
| C7 | 超长函数/过深嵌套 | 风格规范建议拆分和减少嵌套 | 需决定 D4；编译器栈帧警告不等于函数复杂度检查 |
| C8 | 直接分配器误用 | E1 获取清单漂移、E2 定向失败 | 部分已有证据；对象逃逸/释放语义需决定 D2 |
| C9 | 公开/私有注解 | `src/logger_compiler.h`、导出列表、生产制品隔离和 ABI | ELF/公开合同已有证据 E3/E7；全面源码注解 lint 需决定 D4 |
| C10 | 分支提示/always_inline 滥用 | 新增直接 always_inline 受差异门禁限制，抽象层及性能依据由审查负责 | 部分已有证据 E1；分支提示性能依据和全部滥用自动识别需决定 D4 |

`.clang-tidy` 配置文件存在不能单独作为实际执行或覆盖全部上述要求的证据。

## 4. 失败注入矩阵

所有“已有证据”仅指所列模拟路径。统一测试结果格式不是通用故障注入 API；
该 API 是否由链接期包装和隔离钩子替代须经 D3 决定。

| 条目 | 原始要求 | 实际入口与判定 | 分类与边界 |
|---|---|---|---|
| F1 | malloc/calloc | `tests/regression/test_ctor_resource_failure.c`、`run_ctor_resource_failure.sh`：9 个 calloc、1 个 malloc，逐地址释放、原错误、后继创建，E2/E3 | 已有证据；加 3 mutex/2 cond 及健康对照共 16 场景，不是所有分配失败组合 |
| F2 | pthread init/create/join | 构造测试；`run_worker_startup.sh`、`run_worker_failure.sh`、`test_destroy_status.c`，E2/E3 | 已有证据；join 不确定时保守保留，不伪造所有宿主线程已退出 |
| F3 | lock/wait | `test_lock_failure.c`、`test_lockdep.c`、`test_global_lifecycle.c`、`test_queue_wakeup_litmus.c`，E3/E8 | 已有证据；模拟失败不等于对应 libc 在所有环境一定产生该错误 |
| F4 | open/openat | `test_file_acquisition.c`、`run_file_acquisition.sh`，加活动文件既有故障；E2/E3 | 已有证据；4 个文件获取拒绝加健康对照，验证所有实际 fd/DIR 归还 |
| F5 | read/write/短写 | `test_file_backend.c`、`test_io_accounting.c`、`test_syslog_faults.c`，E3；生产 src 无直接 read 调用 | 写/短写已有证据；未采用的直接 read 注入不适用，不声称穷尽 libc 内部读写 |
| F6 | fsync/fdatasync | `test_file_backend.c`、`test_destroy_status.c`、普通日志崩溃合同，E3/E7 | fsync 已有证据；生产 src 无直接 fdatasync，不适用该入口；非物理掉电认证 |
| F7 | rename | `test_file_backend.c`、`test_crash_recovery.c`、`test_rotation_mutations.py`，E3/E7 | 已有证据；普通日志轮转，不是 Audit 事务协议 |
| F8 | entropy | E0 已退出 Audit/密码学 | 不适用；不是熵源失败测试通过 |
| F9 | queue/resource pool | `test_queue_mpsc.c`、`test_queue_properties.c`、`test_worker_failure.c`，E3/E4 | 已有证据；包含真实溢出位图的饱和/归还，不虚构通用资源池 |
| F10 | worker startup | `test_worker_startup.c`、`run_worker_startup.sh`，E3 | 已有证据；真实生产归档、构造与线程交接，不是运行任意宿主取消策略 |
| F11 | teardown | `test_destroy_status.c`、`test_constructor_rollback.c`、`run_global_r7.sh`，E2/E3/E8 | 已有证据；区分实际释放、错误回执与无法证明静默时保留 |
| F12 | refcount/invariant hooks | refcount 不存在；lockdep 负例、Global 精确 CAS 白盒与生产 R7 是实际不变量验证，E3/E8 | refcount 不适用；现有不变量已有证据，精确 CAS 窗口使用 LOCKDEP=1 支持库而非生产 .a 内部暂停 |

生产边界：`logger` 为 LOCKDEP=0/FI=0；回归支持库为 LOCKDEP=1/FI=0；
私有故障库为 LOCKDEP=1/FI=1，不安装、不打包。`scripts/check_production_artifact.py`
与安装测试独立查产物。模糊测试的覆盖插桩归档也不是正式交付归档。

## 5. 对应原始七项验收

| 条目 | 原始验收 | 本次结论 |
|---|---|---|
| A1 | type/lint rules | 需决定 D1；T1–T7 不允许整体勾完成 |
| A2 | semantic pattern checks | 有限定 E1 规则；S4/S5/S7/S8 的完整要求需决定 D2 |
| A3 | style gate | C1/C3/C4 的机械门禁已有证据；其余按 D4 逐项决定 |
| A4 | unified fault injection API | 需决定 D3；没有以既有包装器冒充已实现统一 API |
| A5 | failure matrix | F1–F12 对应的现行适用路径已有证据；没有所有错误码和调度的穷尽声明 |
| A6 | CI gate | E1/E3 实际工作流与源码路径已对应；新增提交仍须自身适用门禁 |
| A7 | 新获取点无未覆盖 rollback | 基线 22/22 登记点有所选直接失败证据 E2/E3；新增/变参会触发审查。全程序/跨函数普遍保证需决定 D2，不能把 22/22 当作无限量词 |

#106 保持开放。D1–D4 接受之前不得宣称原始目标已全部实现；一旦选择本版
实现的未完成项，须登记独立实现单、影响、负责人状态和验收方法，再据实结项。
