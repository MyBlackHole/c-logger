# c-logger 2.0.0 发布前核查与风险签收

> 状态：**尚不批准创建 v2.0.0 标签或 GitHub Release**。这是截至2026-10-17对固定
> 2.0候选的只读工程核查，不是发布授权、不是已发布资产清单，也不替维护者接受剩余风险。
> 该结论特指下列源码身份；后续代码、安装文档或工作流修改须重新判定差异。

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
