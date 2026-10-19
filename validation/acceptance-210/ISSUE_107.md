# #107：测试、模糊验证与性能逐项矩阵

当前事实先读[2026-10-18 发布收敛清单](RELEASE_CLOSEOUT_20261018.md)。
本表保留原 57 项映射及各阶段证据；固定到达率 schema 2、成功调用分位和后续
Sanitizer 数量以当前清单及原始受测源码为准，不能由历史计数推断最新执行。

> **2026-10-17 当前主线复核**：本表从历史PR #215的逐项审查移入最新主线，
> **不修改当时 E0–E9 历史证据的原始SHA、测试规模或阶段结论**。
> 当前审查基线为 `acb7a9978f530a88ca6787af9856237dcea85ce2`，
> 旧基线身份仍仅用于复核旧证据，不代表本次重新运行历史用例。
> 后来已经合并的 fork 运行域、公开ABI状态返回、XPack/安装消费和
> Linux 5.10 验收以 [发布前核查](../../docs/v2/RELEASE_PREFLIGHT.md)
> 与 [当前阶段增量](CURRENT_DELTA.md)为准。D1–D6的提议都没有维护者签收。


固定源码和 E0–E9 身份见 [总索引](README.md)。本表区分已有测试、历史执行、
仍有效的发布检查与未批准的暂缓建议，不以“CI 全绿”替代具体合同。

## 1. 分层与白盒

| 条目 | 原始要求 | 实际入口 | 分类及边界 |
|---|---|---|---|
| W1 | refcount | E0 当前对象没有通用引用计数 | 不适用；不补造不存在原语的测试 |
| W2 | completion | `tests/regression/test_flush_watermark.c`、`test_flush_pending.c`、`test_flush_release_proof.c` | 对象完成谓词已有证据 E3/E8；通用 completion 原语未采用，不适用 |
| W3 | wait_event | `test_queue_wakeup_litmus.c`、`test_queue_notify.c`、`test_worker_failure.c` | 实际等待/唤醒/失败退出已有证据 E3/E8；通用包装未采用，不适用 |
| W4 | bitmap/list | `test_queue_mpsc.c`、`test_queue_properties.c` 与 `src/logger_queue.h` 的 spill_used | 真实溢出池位图占用/归还已有证据 E3/E4；通用链表未采用，不适用；不能将整项删除 |
| W5 | ownership transfer | `test_resource_cleanup.c`、`test_host_ownership.c`、`test_source_ownership.c`、`test_metadata_capture.c` | 已有证据 E3；所有者转移/快照/宿主借用的选定场景，不是所有指针的数据流证明 |
| W6 | state transition | `test_global_lifecycle.c`、`test_worker_startup.c`、`test_destroy_status.c` | 已有证据 E3/E8；实际对象状态及失败阶段，不是完整自动状态空间搜索 |
| W7 | parser | `tests/test_config_abi.c`、`tests/fuzz/public_fuzz.c`、`tests/fuzz/format_fuzz.c` | 现行配置/格式/脱敏域已有证据 E3/E4/E5；无 Audit/检查点解析器 |
| W8 | transaction | E0 不提供审计事务；普通文件轮转见 `tests/test_crash_recovery.c` | 审计事务不适用；有效的轮转/刷新合同仍由 E7 覆盖，不取消 |
| W9 | invariant failure | `test_lockdep.c`、`test_global_lifecycle.c` 精确 CAS、`test_worker_failure.c` | 已有证据 E3/E8；区分生产包装与同源私有断言 |

目录实际分为 `tests/` 生产链接测试、`tests/regression/`、`tests/fuzz/`、
`tests/integration/`、`tests/packaging/`、`tests/scripts/`；白盒并非统一放在
名为 primitive 的目录。构建和链接分层已存在于 `xmake.lua`，不要求为了复制
KUnit 目录形状迁移所有文件。原始“目录分层”按职责分层映射，不借此取消任何测试。

## 2. 安装后公开消费与运行时检查

| 条目 | 原始要求 | 实际入口与证据 | 分类及边界 |
|---|---|---|---|
| U1 | 2.0 公开合同 | `tests/packaging/current_abi_contract.c`、`.cpp`，E7 | 已有历史证据；只用安装公开头与真实静态/共享产物；最终候选仍待 #100 |
| U2 | 默认配置 | `tests/test_config_abi.c` 和安装合同，E3/E7 | 已有证据；未来结构尾部、版本/大小失败码由各自场景检查 |
| U3 | 所有权/错误语义 | `tests/test_api_contract.c`、安装消费及宿主样例，E3/E7 | 已有证据；生产链接内部测试不冒称全部是安装后自测 |
| U4 | 静态/共享包装 | `tests/packaging/check_install.py`、独立 `examples/installed_consumer/`，E7 | 已有历史证据；包含重定位、C/C++11、PIC 模块、pkg-config，非项目 CMake 构建 |
| U5 | ABI consumer | `scripts/check_release_abi.py`、安装 C/C++ 合同，E3/E7 | 当前确认：43 导出/SONAME 2/LOGGER_2.0；安装C11/C++11消费已在PR #235/#236重新执行；仍未批准正式发布 |
| Z1 | ASan | core-validation 的 address-undefined job，E3 | 已有证据；完整静态集合和独立生产包装，不宣称全部共享 DSO 内部插桩 |
| Z2 | UBSan | 同上，E3；格式化独立模式 E5 | 已有证据；各次配置/源码分别记录 |
| Z3 | TSan | core-validation thread job E3、格式化 E5、历史重放 E6 | 已有证据；不会替代 C11 内存序及寿命论证 |
| Z4 | 新原语先有检测/失败证据 | E0 最小充分机制约束，E1 新获取点漂移门禁 | 持续准入规则；当前没有应靠虚构原语填满的待测对象，也不保证未来变更自动正确 |

## 3. 模糊测试与语料

| 条目 | 原始要求 | 实际入口/证据 | 分类及边界 |
|---|---|---|---|
| F1 | Audit parser | E0 产品退出决策 | 不适用，不是测试已完成 |
| F2 | checkpoint parser | E0 无专用检查点 | 不适用 |
| F3 | config/UAPI parser | `tests/fuzz/public_fuzz.c`，E4；E3 亦运行公开模糊入口 | 已有证据：安全同步 stderr 配置、版本/长度/零尾及枚举变异；不把随机字节当非法指针 |
| F4 | record decoder | E0 不存在独立持久/线协议记录解码器 | 不适用该解码器；内部队列记录复制并未取消，见 P4/P5 |
| F5 | formatting/redaction | 公开脱敏 E4；私有静态格式化 E5 | 已有证据：秘密长度至 96、模型精确字节；格式化 UTC 有限日期/容量/哨兵/错误后恢复；非 printf 全语法或任意时区 |
| F6 | persistent decoder | E0 无对应编码/解码格式 | 不适用；普通文件追加/轮转恢复仍见 C7 |
| F7 | corpus + regression seeds | `tests/fuzz/corpus/` 九份、`format-corpus/` 八份；固定 seed E4/E5；Git 原件与重放 E6 | 已有证据：原公开 55、格式化 80 输入重放与原件一致；不宣称样本覆盖全部输入 |
| F8 | 长期探索及失败治理 | [#212](https://github.com/MyBlackHole/c-logger/issues/212) 另有有限预算和正常/失败演练关闭条件 | 需决定 D6；当前未启用定时任务，不把有限 PR 模糊测试当作周期服务 |

格式化四种 seed 各 512 输入、3072 个反例拒绝属于 E5 判定器自检，
不是全程序编译期变异分数。E6 原件恢复和历史源码重新构建也不证明找回历史二进制。
无需重复建设 #208/#209 或重新打开已完成 #211。

## 4. 属性与并发模型

| 条目 | 原始要求 | 实际入口 | 分类及边界 |
|---|---|---|---|
| P1 | encode/decode roundtrip | 无审计/持久格式，E0 | 对该格式不适用；内部队列正文/元数据精确比对见 P4，不误称持久解码 roundtrip |
| P2 | valid state transitions | `test_global_lifecycle.c`、`test_worker_startup.c`、`test_worker_failure.c`，E3/E8 | 确定性正反状态场景已有证据；不是通用随机状态机模型检查器 |
| P3 | refcount terminal state | E0 未采用 | 不适用 |
| P4 | queue ordering/completion | `test_queue_properties.c` 独立顺序 FIFO；`test_queue_mpsc.c`、`test_flush_watermark.c`、`test_flush_release_proof.c`，E3/E4/E8 | 已有证据：六 seed 每个 2500 步与真实并发专项分开；顺序模型不证明并发完成协议 |
| P5 | bitmap allocate/free | `test_queue_properties.c` 长短消息/溢出使用量/排空归零；`test_queue_mpsc.c`，E3/E4 | 已有证据：实际位图资源合同；不是任意通用 bitmap 算法证明 |
| P6 | no-double-release | `test_ctor_resource_failure.c`、`test_file_acquisition.c`、`test_destroy_status.c`、`run_global_r7.sh`，E2/E3 | 已有证据：跟踪真实地址/fd/free/closedir；不得推广到全部可执行路径 |
| C1 | publication | `test_global_lifecycle.c`、`run_global_r7.sh` 和 E8 证明 | 已有证据；精确 RUNNING CAS 与普通生产库 R7 是两条证据链 |
| C2 | refcount 并发 | E0 未采用 | 不适用 |
| C3 | queue 并发 | `test_queue_mpsc.c` 八生产者、`test_queue_wakeup_litmus.c`、`test_queue_notify.c`，E3/E8 | 已有证据：指定交错与 C11 论证；无穷调度未机器穷尽 |
| C4 | completion 并发 | W2/P4 的完成水位、预留空洞及失败退出 | 已有证据 E3/E8；空队列不等于已刷新完成 |
| C5 | seqcount | E0 未采用 | 不适用 |
| C6 | RCU/SRCU（若采用） | E0 使用读写锁生命周期保护，不采用 RCU/SRCU | 不适用；不得因此取消 Global 读者退出保护 |
| C7 | crash/powercut | `tests/test_crash_recovery.c`、`tests/test_crash_contract.py`、`scripts/check_crash_matrix.py`、E7 | 八个 SIGKILL 切点及判定器正负测试已有历史证据；物理 powercut 已退役，不适用。最低内核 VM 是另一个兼容性合同 |

## 5. 性能纪律与发布基线

| 条目 | 原始要求 | 现有证据 | 分类及下一动作 |
|---|---|---|---|
| B1 | 同 runner before/after | E9 历史 #116 直接对照，明确 ABI 1；现有运行器保留固定比较参数 | 工具/历史证据已有；不是当前 ABI 2 的优化收益。新性能修改必须另做同源同条件对照 |
| B2 | 固定 workload | `tests/bench_matrix.c`、`tests/bench_fixed_rate.c`、`scripts/benchmark_fixed_rate.py` | 已有工具和 E9 历史运行；本版最终重测范围需决定 D5 |
| B3 | P50/P95/P99 | 固定到达率工具区分调用服务时延、计划到达延迟、排空和批量 flush | 已有 E9 历史证据；bench_matrix 不提供这些分位，不补造数值；当前候选未重测 |
| B4 | throughput | 三种基准各自的 attempted/accepted/completed 定义 | 已有 E9；不同统计口径不能互换，也不替代真实落盘吞吐 |
| B5 | drop/fallback | 入队、丢弃、同步回退和完成计数；`tests/scripts/test_fixed_rate_benchmark.py` 验收器 | 已有 E9；不可通过多丢弃或弱化语义伪造收益 |
| B6 | fixed memory | 预分配队列、溢出池和 workspace 字节及矩阵记账测试 | 已有 E9；固定逻辑存储不等于宿主 RSS/分配器全部开销 |
| B7 | object sizeof | `bench_matrix.c` 私有生产静态观测；`tests/scripts/test_benchmark_build.py` 构建边界检查 | 已有工具及 E9；历史 sizeof 不是所有 ABI/编译器通用值 |
| B8 | cacheline/false-sharing | 源级写者图见下一节；目前没有针对本基线的缓存行失效计数实测证据 | 需决定 D5；不声称源级分析等于硬件性能剖析，不凭分析增 padding |
| B9 | 可复现基准基线/原始记录 | E9 报告与摘要；部分原始延迟数组未在 Git 保存 | 历史有限记录存在；最终候选基线工作负载及原始保存需决定 D5，不将 E6 模糊归档冒称性能归档 |

## 6. 热共享对象的源级访问图

以下是本基线源码的静态审查，不是 offsetof/缓存行探测，不承诺实际结构布局
或设备性能；建议的冷热分类只用于定位将来测量对象。本次不修改对齐和 padding。

| 字段族与来源 | 主要写者/读者 | 同步或生命周期约束 | 热冷与潜在共享位置 |
|---|---|---|---|
| queue slots/cap/mask/spills/spill_cap/storage_bytes，`src/logger_queue.h` | 构造者写一次，生产者/消费者读 | 发布前初始化、销毁前静默 | 只读配置与经常访问的索引相邻；具体缓存行需实测 |
| enqueue_pos / dequeue_pos | 多生产者预留；单消费者推进 dequeue，深度/诊断可读取 | 原子位置不单独发布槽位负载 | 热计数、多核共享可能产生竞争；不假定已有隔离 |
| slot.seq / slot.record | 生产者发布、消费者读取/归还 | seq 的发布/复用代次保护 record | 热生产/消费交接；record 内大小不代表固定缓存行布局 |
| spill_used / spill.text | 生产者 CAS 取得位并填充，消费者复制后清位 | 结构性所有者为队列，槽位交接后复用 | 热位图 RMW 可能共享；不是每个生产者独立位图 |
| consumer_waiting / wait_mu / wait_cv | 消费者布置/复查，生产者发布后探测/通知，控制者强制唤醒 | E8 RMW 与锁内谓词协议 | 热慢路径共享；不能为减少流量改为普通 load/store |
| wait_count / producer_wake_signals / force_wake_signals | 消费者/生产者/控制者各有主要更新入口；观测方读 | 原子诊断量，不负责生命周期 | 统计写入与协议字段相邻的成本需测量 |
| Logger level/state、配置与后端，`src/logger_internal.h` | level/state 多方原子读写；固定配置构造后读；后端由输出路径更新 | 生命周期保护、emit_mu，不能把固定配置规则套给动态状态 | 固定配置偏只读；状态、输出和计数较热 |
| completed_pos / progress_mu / progress_cv | worker 完成路径推进，flush 等待者读取 | 受 progress_mu 和完成谓词保护 | 热完成交接；不是 dequeue_pos 的同义量 |
| worker_workspace、文件/Syslog 状态 | workspace 工作线程借用；后端由同步调用方或 worker 在输出锁下更新 | join/静默之后销毁，后端 emit_mu | workspace 工作数据热；路径/初始化元数据相对冷，重开/轮转仍会访问 |

原始四类要求 producer-written / consumer-written / read-mostly / hot-cold
均已给出源级对应，但本表不声称硬件 false-sharing 分析已经完成。
现有“只有基准证明才引入 padding/alignment”的准入规则继续有效。

## 7. 对应原始九项验收

| 条目 | 原始验收 | 本次结论 |
|---|---|---|
| A1 | 测试目录分层 | 职责和三种链接边界已存在；见第 1 节及 E3/E7，不为目录外观重写测试 |
| A2 | primitive white-box | 现行实际原语/对象合同 W1–W9 已映射；不采用的对象不补造 |
| A3 | public selftests | U1–U5 有历史安装消费证据；最终候选见 A9 后的独立发布项 |
| A4 | sanitizer matrix | Z1–Z4 有对应 E3/E5/E6；明确静态/支持库边界 |
| A5 | fuzz corpus | F1–F7 当前适用域有 E4/E5/E6；周期服务需决定 D6 |
| A6 | property suite | P1–P6 有限模型/确定性场景已映射；不冒称完整模型检验 |
| A7 | concurrency litmus suite | C1–C6 有指定交错和 E8 推导；未采用的原语不适用 |
| A8 | crash/powercut suite | C7 八个进程切点独立覆盖；无物理掉电认证 |
| A9 | reproducible benchmark baseline | E9 历史工具/运行存在；当前发布基线、缓存行实测与原始数据保存需决定 D5 |

**本版必需未完成：#100 的最终固定候选同源验收及独立发布授权。**
具体包含生产静态/共享构建、包校验、ABI、重定位安装消费及最低支持环境。
不能以 #214 的七项适用工作流替代未触发的 release-validation/minimum-kernel，
也不能以原始语料重放代替当前候选的正常构建。#107 保持开放，D5/D6 的本版
边界须接受，判定出的必需缺口须完成；不无限扩大一次性测试建设任务。
