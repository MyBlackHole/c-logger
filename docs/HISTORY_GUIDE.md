# 历史材料与保留原文导读

本文帮助区分现行说明、阶段记录和原始证据。下列文件中的“当前”“已通过”“生产版”等措辞，应连同该文件的版本、基线提交、日期及验证范围阅读，不能直接套用于今天的主分支。

## 先读现行入口

当前使用与验证方式见 [README](../README.md)、[API](../API.md) 和 [TESTING](../TESTING.md)。并发与所有权规则从 [v2 文档入口](v2/README.md) 阅读；Audit 退役范围见 [迁移说明](v2/AUDIT_REMOVAL.md)。这些入口的支持范围也不能代替实际部署验收。

## 按用途查阅历史材料

以下路径相对源码仓库根目录。安装包只携带安装清单列出的文档，未附带的历史材料应到对应版本的源码树查阅。

- 旧入口快照：`docs/HISTORY_README.md`、`docs/HISTORY_API.md`、`docs/HISTORY_CONTROLLED_FORK_REINIT.md`。其中提到的旧接口或受控 fork helper 不能据此视为当前公开能力。
- 版本与第一代边界：`docs/RELEASE_NOTES_0.9.1.md` 至 `docs/RELEASE_NOTES_0.9.6.md`、`docs/RELEASE_NOTES_1.0.0.md`、`docs/V1_KNOWN_ISSUES_HISTORY.md`、`docs/V1_RELEASE_CRITERIA.md`。保留各版本原有的发布身份与限制，不用主分支的 ABI 2 状态改写旧结论。
- 阶段修订：`docs/ROUND1_QUEUE_FLUSH.md` 至 `docs/ROUND5_FORK_ISOLATION.md`，以及 `docs/GLOBAL_LIFECYCLE.md` 中的演进背景。按文中输入、基线和阶段范围理解，现行约束以对应当前入口为准。
- 已退役 Audit 的设计记录：`docs/v2/AUDIT_FORMAT_FAILURE.md`、`AUDIT_RETIRE_OWNERSHIP.md`、`AUDIT_SEQUENCE_CONTINUITY.md`、`AUDIT_TRANSACTION_SEMANTICS.md`、`AUDIT_TRANSACTION_SUBMISSION.md`。保留其当时语义以供追溯，不恢复已退役功能。
- 实施计划：`docs/superpowers/plans/`。计划、待办及勾选状态是过程材料，不是完成验证的独立证明。
- 历史验证记录：`validation/` 中的 `AUDIT_TEST_RETIREMENT.md` 及各专项验证 Markdown，按各自基线保留原始通过、失败、环境限制、样本和命令；一项测试通过不能外推为全套发布或真实硬件验收。
- 当前验证清单：`validation/cas_orders.tsv` 是仍由 CAS 机械门禁使用的活跃清单，应随相关代码变更评审并更新，不能当作冻结的历史原始结果。

## 中文化时保留哪些原文

- 原始测试日志、基准数据、表格字段、提交哈希、时间、命令和错误输出保留；可在旁边增加明确标注的中文解释。
- `LICENSE`、版权声明、SPDX 标记及第三方通知保留原文。
- API/ABI、协议、参数、状态值、机器可解析输出和 CI 身份保持兼容。具体边界见[项目语言约定](PROJECT_LANGUAGE.md)。

保留原文是为了可追溯与兼容，不豁免普通维护说明的中文化，也不表示历史结果已经重新验证。
