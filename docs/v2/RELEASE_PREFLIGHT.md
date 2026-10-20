# c-logger 2.0.0 发布前核查与风险签收

> 状态：**尚不批准创建 v2.0.0 标签或 GitHub Release**。当前核对基线更新至
> 2026-10-18 的 #272；下方 2026-10-17 各节保留其历史源码与运行身份。
> 本文记录工程事实和待决范围；后续代码、安装文档或工作流修改须重新判定差异。

## 2026-10-18 当前核对结论

核对 main 为 `7793607ff0c433959cc6a9a558d0b6536a8622d2`，
tree 为 `39b73274f1afb91eafdbecf8007953822a79668c`。
VERSION 为 2.0.0，SONAME 为 `liblogger.so.2`，`LOGGER_2.0` 仍有 43 个导出。
本文件所称核对基线尚不是正式冻结基线；签收 JSON 仍为 12 项 pending、冻结字段为空。

[#272](https://github.com/MyBlackHole/c-logger/pull/272) 的 PR head
`a5c35c0cf74ea4cb0d452259fc166117a22cc216`、受测合并
`80c5edf536b1bfffc0f689c67e15c0bb2dcd6c8a` 和上述 main 的树一致。
PR 的 19 工作流／53 作业全部成功；合并后 main 的 15 push 工作流全部成功，
对应 46 成功作业、1 个 PR 条件跳过。PR 专属发布／最低内核／Sanitizer 等证据
仍属于各自原始源码，不能写成 main 上重新执行。

| 当前事实 | 仍需明确的边界 |
|---|---|
| 公开状态接口迁移已经完成，静态完整配置 1040、共享发布配置 803 项通过 | 公开 ABI 的最终冻结与正式候选包验收仍独立进行 |
| 高 fd 的 GCC／Clang × 生产静态／支持归档 × grow/grow-failure 共 8 次真实成功 | 指定 ENOMEM 失败路径、旧登记后继可用和最终回收有证据；不推为全部错误／调度或共享 DSO 内部注入 |
| TSan 原件为 1040 注册、1009 实际执行成功、31 明确能力跳过；ASan/UBSan 1040 通过 | 31 项不是已覆盖，也不是全部子进程检测能力限制的穷尽计数 |
| 原始审计的 9 个同步 fork 场景确有成功标记 | 仅称 TSan 构建下真实运行；同步子方不等于 fork 前父方单线程，尚不能证明子方检测全部开启 |
| 独立模型真实取得健康 0、锁序错误 66、竞争错误 66 | 模型的单线程 fork 前提与有限通知范围不能转用于全部 Logger 子方场景 |
| #265/#266 已补性能逐条返回值、成功分位、文件集合及预算／归档失败保留 | 既有结果只属于其受测源码；不冒称全部绿色工作流含最新候选固定到达率专项 |
| #267–#272 已补直接字节复制 AST、UBSan 失败判定及四组有限类型约束 | 索引／计数、标志类别、类型擦除／间接别名等残余按实际对象审查，不能整组勾完或重做已有能力 |

TSan 覆盖限定有具体机制依据：GCC 13.3 上游
[`ForkChildAfter`](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.3.0/libsanitizer/tsan/tsan_rtl.cpp)
对多线程父方 fork 后的子方启用报告和访问忽略。当前同步场景的父方仍可有 worker／
持锁线程；既有审计没有证明其单线程前提。上游源码只用于说明边界，未将其当成
Ubuntu 运行库的逐字身份认证；没有项目设置 `die_after_fork=0` 也不足以证明有效
运行库的全部默认策略。原有 31 项明确跳过及模型正反例均保留原始含义。

剩余工作继续按既有 R1–R5 映射收口，当前完整清单位于源码树中的
`validation/acceptance-210/RELEASE_CLOSEOUT_20261018.md`：宿主／同文件单所有者
合同、检测器能力、有限工程范围、性能证据和冻结／资产。根据
[现有发布工程](../RELEASE_ENGINEERING.md)，未另行约定的业务 SLO 不新增为代码缺陷，
也不臆造绝对吞吐／P99 数值；D5 对实际测量范围与未承诺指标的签收仍保留。

本轮准确 main 的补测在静态第 8 个样本后遇到清理错误，runner 退出 1，汇总只有 7 行。
原失败记录保留，结果记录路径按 [#273](https://github.com/MyBlackHole/c-logger/pull/273)
独立修复；新候选的实测和 CI 按其准确身份另行记录，不能将原失败轮次改记为通过。

本轮远端读取的 rulesets 集合为空；传统 main 保护接口返回
`403 Resource not accessible by integration`。这只表明当前读取权限不足，
不能据此推断传统分支保护已开启或未开启；正式发布前仍按现有发布权限要求核实。

以下 2026-10-17 文本保留阶段证据。涉及旧接口、旧计数、未合并候选和高 fd 未测等
描述时，以本节及当前源码为准；不把修改文档视为重新执行旧测试或接受剩余风险。

## 2026-10-17 后续阶段增量（不覆盖下方历史基线）

在 [PR #248](https://github.com/MyBlackHole/c-logger/pull/248) 通过全部 14 项适用 CI
之后，主线已推进到 `d33666106a000aa8401b9e83d1cf6018505f9f6a`。
其最后一轮源码 head 为 `fe918484839af88ecfa4d3f1722ad051ea2c69cb`，
受测合并提交为 `a2e2fabadf99b235fa834aa9b408edb3d30b6e53`。
GCC9.4 与 Clang10 各完成 **995/995** 项完整普通静态回归，0 失败；
[工程基线](https://github.com/MyBlackHole/c-logger/actions/runs/39026794135)
和 [Linux 5.10.271/glibc2.31/ext4/XFS](https://github.com/MyBlackHole/c-logger/actions/runs/39026794212)
均有精确提交回执，后者原始产物 ID `12099505515`。

当前 `abi/logger-2.0.symbols` 确认 **43 个非注释条目**，公开 ABI 未因
PR #240/#242/#244/#246/#248 的私有 fd 拥有权路径收敛而变化。
相应改动提升了词法关闭/失败路径的一致性，**并不等于完成 D1 的七类单位隔离、
D2 跨函数 AST 分析、#221 的 31 项 TSan fork 检测器覆盖或 #218 的宿主范围签收**。

当前新准备工作是复用生产 `bench_fixed_rate` 和严格原始校验器，
为最终候选提供有预算的吞吐、丢弃与 P50/P95/P99 观察证据。
新增工作流的成功/数据需以实际精确提交和原始制品为准；
不得从这段计划推断性能已经测出或业务 SLO 已经达标。

下文 §1/§2 的 `26bbd7dc` / PR #235 是当时**固定历史候选**，
不将其原始 952 项回归或旧 SHA 改写成当前主线的新执行。A–F 的风险签收仍未获得批准，
`v2.0.0` 标签与正式 Release 仍不得擅自创建。

## 2026-10-17 高 fd 独立补验（有明确环境边界）

此前的 fork 高 fd 位图扩容成功/失败两条场景在早期容器硬限额 16384
下仅有退出77的跳过回执，不能叫作已通过。现有
[PR #250](https://github.com/MyBlackHole/c-logger/pull/250)
新增一个**不接受跳过**的专项入口：
[GitHub Actions #39029945235](https://github.com/MyBlackHole/c-logger/actions/runs/39029945235)，
GCC 和 Clang 在 Ubuntu24.04 宿主（`RLIMIT_NOFILE` 软/硬限额 65536）
分别对真实生产静态库和白盒归档执行 `grow`、`grow-failure`，
合计 **8/8 成功、0失败、0跳过**。真实运行源是受测合并提交
`3da308e1eda20cbef036c34b01045f0f818f2491`；
两份原始产物 GCC `12099722524`、Clang `12100191695`
包含源tree、构建日志、二进制摘要、每场景退出码和fd限额。
若 PR 后续修改，必须按最终 head 重新校验，不复用旧结果为新运行。

这补齐**特定高 fd 位图扩容路径的真实测试**，不扩大最低支持平台，
不推断任意宿主库调用的多线程fork合法性；GCC TSan所限的31项 fork
后worker覆盖、D1–D6风险签收与高负载业务SLO仍是独立未决项。
下方分组D的旧“部分高fd未实测”记录是当时基线状态，
不代表本次证据已获正式发布签署。

## 1. 精确源码与版本身份

| 对象 | 已核实事实 |
|---|---|
| 只读审查时的 `main` | `26bbd7dc7959d1779ee8310fe19c7b5d91b5e96f` |
| `main` 完整 Git tree | `c164a95ef1c22db5243dc38865183ba22b34d3f4` |
| [PR #235](https://github.com/MyBlackHole/c-logger/pull/235) 最终功能 head | `4f8309fd90f0b307888f663597e47c69f5af7d3b` |
| PR 受测合并身份 | `c213932574bb1de2647693b58b29ce6c04c70c42`，两个父提交分别是前主线与PR head；**tree与以上两者相同** |
| `VERSION` / SONAME / ELF 版本节点 | `2.0.0` / `liblogger.so.2` / `LOGGER_2.0` |
| 精确符号清单 | `abi/logger-2.0.symbols`，**43个非注释条目**；名称数量不意味着旧开发期 ABI 函数类型可混用 |
| 正式身份 | 查无 `refs/tags/v2.0.0`（404），GitHub Releases 中无2.0版本；已发布 v1.0.0 不动 |

`main` 的合并提交与PR head提交号不同但 tree 完全相同；下表
“PR准确源码执行”的结果据此具备代码/构建输入等价性。**该等价性不会自动扩展到
以后编辑的文档、工作流、安装包内容或正式发布资产**。此审查文档本身合并后
主线 tree 将改变，不得把这里的旧 tree 当作新主线身份。

## 2. 已有真实工程证据（不计入发布动作）

| 门禁 | 原始出处、精确结果与范围 |
|---|---|
| [完整普通静态](https://github.com/MyBlackHole/c-logger/actions/runs/39013050249) | 952/952 实际成功，0失败/0跳过 |
| [静态与共享 XPack](https://github.com/MyBlackHole/c-logger/actions/runs/39013050262) | 生产静态952/952、共享组合753/753；Ubuntu 20.04基线打包、静态/共享安装验收及Ubuntu 24.04独立下游消费成功。共享配置的包装测试仍使用支持静态归档，不声称拦截DSO内部 |
| [ASan+UBSan](https://github.com/MyBlackHole/c-logger/actions/runs/39013050300) | 完整静态952/952实际成功，运行库正反资格继续保留 |
| [TSan](https://github.com/MyBlackHole/c-logger/actions/runs/39013050300) | 总注册952，**921实际通过，31既定fork后线程场景明确跳过（5+13+13），0失败**；新增24个级别测试没有跳过。Xmake把退出77的skip-equivalent计入聚合成功，不能写成952项实际通过 |
| [真实Linux最低内核](https://github.com/MyBlackHole/c-logger/actions/runs/39013050268) | Linux **5.10.271** / Ubuntu 20.04 / **glibc 2.31** 客户机实际完成ext4、XFS、随机源和生产共享库运行探针；证据 `source_commit=c213932574bb1de2647693b58b29ce6c04c70c42`，tree与PR head一致 |
| [其余工程矩阵](https://github.com/MyBlackHole/c-logger/pull/235/checks) | PR最终15/15工作流、40/40作业成功，包含fork运行域、注册边界、真实Syslog、崩溃、模糊测试、源码格式、ABI、构建版本与队列基准 |
| 主线合并后 | `26bbd7dc` 的11个相关`push`工作流成功；正式 `release-publish` 未触发，不能将这些push检查充当标签发布验收 |

原始、可校验的两个有限证据：

- `sanitizer-evidence-thread` ZIP SHA-256：
  `36206f58e77ecee86b836b9f423829fe9bbb6380830857f56408d45a3eba2734`。
  制品内 `source-commit.txt`、`source-tree.txt` 分别为上表PR head和tree；
  `test-exit-code.txt` 为0；`xmake-tests.log` 要独立区分skip回执与通过。
- `minimum-kernel-linux-5.10` ZIP SHA-256：
  `1303072611ce3457109f91567356aee2139ae4136e6840756d5805ea9dcfb621`。
  `platform-evidence.txt` 明确含5.10.271、glibc2.31、ext4/XFS所需探针与准确
  `source_commit`，不是从ELF GLIBC版本反推内核兼容性。

这些证明的是特定构建和平台上的实际结果，**不是任何Linux、任意宿主、任意调度、
物理断电或下游具体业务吞吐SLO的保证**。无等价配置的性能比值不写成优化收益。

## 3. 真正尚需签收的决策（本文件不能代签）

| 分组 | 当前判定 | 发布前需要的明确决定 |
|---|---|---|
| A：公开ABI冻结 | 接口错误形式与43导出/静态共享消费已有证据；**尚未获最终冻结批准** | 确认在获批固定提交上不再改变公开函数类型、配置布局、SONAME与符号版本；确认调用方以同版头/库重编 |
| B：fork多线程/宿主libc | 可信普通libc fork与子方显式新域有真实功能证据，但非通用POSIX fork恢复器 | 宿主明确接受：父方旧指针在子方失效；只在有效快照、fork返回后普通上下文重建；外部stdio、区域设置、动态装载、自定义printf/分配器状态不受库保护；原始系统调用、`_Fork`、`vfork`及信号场景不在支持范围；或将具体必需场景登记为阻断实现 |
| C：TSan覆盖 | **31项既定能力限制跳过**，不是31项代码通过或已证明无竞争 | 确认已有独立线程/锁/ASan、实际父子用例可作为该有限Linux/glibc范围的替代证据；#221及试验性[PR #227](https://github.com/MyBlackHole/c-logger/pull/227) 不因此自动完成或合并。若要求31项全部TSan动态覆盖，则该版本继续阻断 |
| D：高FD/其它功能边界 | 部分fork fd扩容测试受运行器硬限额约束；同一目标不支持多个独立进程共用轮转 | 维护者决定是否将高FD扩容直接实测列为本版必须；若必需，应在满足句柄上限的平台实测，不得将受限环境的skip写为通过。对同文件多进程独立写入继续维持单所有者EBUSY合同 |
| E：工程范围收敛 | [#106](https://github.com/MyBlackHole/c-logger/issues/106)、[#107](https://github.com/MyBlackHole/c-logger/issues/107)、[#210](https://github.com/MyBlackHole/c-logger/issues/210) 尚开放；D1–D6尚未逐项获维护者明确接受 | 逐项判定“当前版本必需的正确性缺口”或“可列明风险的后续增强”。[PR #215](https://github.com/MyBlackHole/c-logger/pull/215) 的矩阵与[PR #217](https://github.com/MyBlackHole/c-logger/pull/217) 的有限fd类型实现仍未合并，不将其绿灯自动当作main能力；也不得将所有原始wishlist机械标为发布阻断 |
| F：最终资产和人工授权 | 根版本号有、发布链存在，**无v2标签和正式Release/四件正式资产** | 单独批准最终完整源码SHA、风险声明、release notes与标签操作；用现存获批标签运行正式流程并核对四件资产的SHA-256、标题/稳定类别及来源；正式发布前不可声明已交付 |

特别说明：已退出的 Audit、密码学审计链、QEMU物理掉电认证不属于当前普通Logger
2.0产品，不能把被删除的测试重新当成必过门槛，也不能称其已通过。
[#212](https://github.com/MyBlackHole/c-logger/issues/212)的周期模糊治理是否为本版
必需，属于E类范围决定；不开后台无限测试来代替一次性的可证伪验收。

## 4. 发布过程的不可逆边界

`.github/workflows/release-publish.yml` 的 **`push: tags: [v2.*]` 会在推送
`v2.0.0` 标签时直接启动正式发布流程**；`workflow_dispatch`只能指定现存v2
标签，也不是无副作用的候选测试按钮。发布源被固定到标签解析的commit SHA和
标签对象ID，构建前及第一次发布写操作前复核，但远端标签并发移动仍存在非原子
检查/写入间隙，发布窗口需要禁止标签变更。

因此：

1. 本审查阶段**禁止**创建临时、试验、正式 `v2.0.0` 标签；不运行
   `release-publish`，不把当前 `main` 或PR合并等同于正式批准。
2. 完成A–E风险选择与必要修复后，固定新`main`完整SHA/tree，核对
   `VERSION`、导出清单、安装文档、全部适用的CI与源码/工具/平台一致性。
   同树旧PR证明必须注明等价承接且没有重新执行，输入变化则重跑。
3. 维护者**另行明确授权**指定提交的稳定标签与发布；在受控无并发重打标签窗口
   执行。标签推送会触发正式发布工作流，不需要再通过造试验标签检测触发器。
4. 从CI获得真正的正式静态/共享TGZ与各自SHA-256（合计4项），下载回验，
   同时核对 `SOURCE_COMMIT`、标签对象身份、ELF和消费方。
5. 只有远端Release及资产复核完成后才能写“2.0.0已正式发布”；分支合并、
   版本号、历史回归或预先XPack均不足以满足此条件。

详细失败关闭与校验路径以[发布工程](../RELEASE_ENGINEERING.md)和
[发布安全](../RELEASE_SAFETY.md)为准。若A–F未签收，**当前结论为不创建标签，
而不是声称全部工程检查失败**。[#218](https://github.com/MyBlackHole/c-logger/issues/218)
和[#221](https://github.com/MyBlackHole/c-logger/issues/221)保持开放，不通过文档强制关闭。

## 2026-10-17：TSan skip-equivalent 的准确口径与候选审计（#251）

`sanitizer-evidence` 的 Xmake 完整静态测试必须区分
**注册数、实际执行成功数、明确因 TSan 限制而跳过数**。
主线 #250 的受测提交 `557098ee59bff8e3558709c3d0f40ef3e3e0602d`
原始日志为：TSan 注册 995 项，实际执行成功 964 项、因 post-fork-worker
限制跳过 31 项；ASan/UBSan 对照 995 项实际执行成功（0 跳过）。
现有独立 TSan 映射模型的健康/真实锁逆序/真实数据竞争
运行退出码为 0/66/66，不能当作额外31项的动态检测覆盖。

新 `scripts/audit_fork_tsan_coverage.py` 与对应独立 CI 后置 job
要求源代码提交和树一致、跳过的31个具体标签与原因完整、
ASan/UBSan 的相同31项有真实成功标记、TSan另9项同步锚点真实执行、
检测器三个资格回执仍能拒绝真实错误。**只有对应准确源码的审计工作流
实际成功，才可将此列为新的发布工程门禁证据**。它不自动授权
TSan受限线程模型的剩余风险；#221保持开放，不改变正式发布所需人工决策。

## 2026-10-17 fork后worker 独立生产压力证据（#253，待独立验收）

在 `#252` 固定了 TSan 的31项能力跳过口径以后，新增
`fork-worker-stress-validation` 作为不使用 TSan 抑制、
不转化既有 TSan skip 的**另一种类型的验证**：
生产静态/共享库与普通/ASan-UBSan的4种组合，父方两个并发写者、
串行/并行四个子进程，每个子方重建运行域并创建真正异步 worker，
以原始文件记录及 I/O 指标核对父子隔离和完成。
判定器要求每组合6轮真实成功，任意77、缺成功标记、错误输出或超时失败。

此举不修改生产代码/公开ABI/最低平台支持基线。
**只有本 PR 精确 head 的专项及适用CI实际成功、保存全部原始制品后，
才是有效新增工程证据**。新增测试不证明多线程fork后由GCC TSan
动态检测了那31项；#221、#218的宿主环境边界及正式发布风险接受
继续由维护者独立决定。

## 宿主外部 FILE 锁受控测试（#255；不代替宿主风险签收）

已加入`fork_host_file_boundary`专用程序，保留独立宿主FILE所有权：
父方外部线程持有独立FILE流锁或先释放（正向对照），子方仅检验
宿主原始fd仍开放，显式初始化自己新的Logger运行域及独立文件输出，
不使用或释放继承的外部FILE指针；父方验证宿主FILE可继续使用，
原始父子文件内容隔离。预计的专项验证为现有静态/共享 × GCC普通 /
Clang ASan/UBSan 4种身份，每身份2模式各3轮；审计必须以**准确
源码head的真实工作流成功和原始证据**为准，未运行前不得勾选通过。

本场景不会覆盖宿主其他线程直接持有locale/tzset/dlopen、
自定义printf扩展、外部分配器等隐藏锁时的重建合法性，
也不会解除TSan的31项动态检测缺口或改变Linux/glibc支持条件。
只有显式人工记录已接受的范围与残余风险才有发布签收效力。

## 2026-10-17 发布范围签收的机器拒绝门禁（#257）

已将**风险接受记录缺失**从人工注意事项提升为发布前明确失败条件，
不变更既有精确标签、ABI、打包、安装和线上资产校验：
`.github/workflows/release-publish.yml` 在从远端绑定`v2.x.y`标签提交，
验证 `VERSION`、ABI 2 和符号清单之后，**只有**
`scripts/check_release_signoff.py` 核验通过，才写入
`authorized=true` 并启动任何正式打包或 GitHub 发布调用。

唯一发布签收输入是标签提交内
`validation/release_signoff_v2.json`。
目前它的 **A–F 六项 + D1–D6 六项全部为 `pending`**，
`frozen_base_commit=null`，因此当前`main`即使有人意外推送
`v2.0.0`标签也应在`prepare`明确**失败关闭**，绝不能自动把
缺失的批准推断为同意。该文件在没有维护者真实决定前
**禁止由工具自行改为`approved`/`accepted_with_risk`**。

将来另行获得人工批准时，需先完成全部生产源/公开ABI/CI/文档冻结，
确定一个**已完整测试、签收尚未修改**的冻结基线Git SHA；
从该SHA到最后被标记为`v2.x.y`的Git提交，**仅能改动此JSON签收文件**。
对每项记录维护者GitHub账号、UTC时间、对应跟踪Issue的实际决策评论、
独立工程证据URL和具体的剩余风险；没有批准、任何一项`pending`、
未追溯的接受人/证据、冻结后代码或CI工作流变化，都不能取得机器通过回执。
如需改动生产代码、测试或发布脚本，应重新冻结、重新审查，
不能通过编辑声明绕过源变更。

**权限边界**：JSON与Git审计只能验证记录的完整性及源码漂移，
不能认证Issue评论的真实作者或核实维护者的GitHub权限。
本仓库由一名维护者管理；`main`要求PR、三项无条件CI检查与已解决的审查线程，
`v2.*`标签限制创建且禁止更新和删除。GitHub PR批准数为0，
维护者不能以自己的PR批准构成独立复核。发布前由维护者本人核对精确候选提交、
真实CI、制品和每项决策评论，并在[#285](https://github.com/MyBlackHole/c-logger/issues/285)
记录缺少第二人审查的剩余治理风险。管理员仍能修改仓库规则，
因此规则配置和自审记录均不能证明独立复核已经发生。
禁止把本门禁的`RELEASE_SIGNOFF_VALIDATED`当作这些权限检查的替代，
更不得以单独修改JSON产生的机器绿色回执宣称业务SLO已满足。
本次不会推送标签、调用`release-publish`或创建正式资产；
已有`v1.0.0`维护线不受影响。
