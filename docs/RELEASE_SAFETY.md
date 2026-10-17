# 显式发布门禁与安全迁移顺序

阶段 A 只关闭 `main` 自动发布入口并验证显式发布来源；版本与包标题由独立的阶段 B 迁移。
两阶段均不修改仓库权限或机密配置，也不代表批准任何正式标签、手动触发或发布。

## 门禁合同

- `push` 只订阅 `v2.*` 标签；普通 `main` push、`VERSION`、发布说明与本工作流
  自身的合并都不是发布事件。`prepare.if` 另行拒绝分支 push、标签删除及其他事件。
- 手动 `workflow_dispatch` 必须填写现存的 `v2.x.y` 标签。这里的字符串输入不是自动创建
  标签的请求；空值、分支、预发布后缀及非规范数字版本都失败关闭。
- 实际 shell 重新检查事件和格式，从 `origin` 获取精确标签，再要求它可解析为提交。
  标签 push 的提交还必须与事件的 `GITHUB_SHA` 一致。手动触发分支的 `VERSION`
  不参与发布：验证与构建读取选定标签的提交。
- 选定提交必须具有相同 `VERSION`、ABI 2、`LOGGER_2.0`、配置的
  `abi/logger-2.0.symbols` 和非空符号条目。已有构建/产物检查仍负责验证实际 ELF、符号
  允许列表、SONAME、生产隔离与完整包内容；来源门禁不能代替它们。
- `prepare` 成功且产生门禁通过标记后，才允许下游执行。构建与发布检出
  固定提交 SHA，不使用可移动的标签名作为检出引用。构建前与第一次发布
  写操作前，都重新获取远端标签，比较提交 SHA **及标签对象 ID**；因此同一提交
  的附注标签被替换也会被拒绝。
- 只有一个 `gh release create` 路径，且必须使用 `--verify-tag`。不再存在缺失标签时
  回退到分支、`GITHUB_SHA` 或 `--target` 自动造标签的路径。已有发布的修复同样
  经过来源复核。

## 必须遵循的迁移顺序

### A. 单独合并门禁

1. 先审查门禁 PR 和本地/PR 测试；该 PR 保持 `VERSION` 和包标题不变。
2. 合并前记录 `main` SHA、现有发布与资产基线，并读取仓库 Actions 运行记录的
   `queued`、`in_progress`、`waiting`、`pending` 状态，筛出 `release-publish`。
   若存在旧发布活动，暂停合并并报告；不要擅自取消发布或改变仓库权限。
3. 合并门禁 PR。GitHub 按事件关联的提交/ref 读取工作流，因而该合并产生的
   `main` push 使用**新工作流**，不会根据前一版的 `main`/path 触发器发布。
4. 合并后确认远端 `main` 已包含门禁，查询该精确合并 SHA 对应的工作流运行记录，
   确认没有 `release-publish` 运行；再次检查旧活动运行，并把发布/资产 ID、名称、
   大小、更新时间以及可用的摘要与合并前基线比较，确认没有发布变化。
   这一步需实际 GitHub 只读证据，不能由本地测试替代。

### B. 另一个 PR 才迁移版本和标题

只有 A 的合并及线上无发布副作用核验完成后，才在另一个 PR 中迁移 `VERSION`、包标题
和相应文档：`VERSION=2.0.0`，XPack 标题与 pkg-config描述为 `ABI 2 development`。
这只对齐开发身份，不代表公开 API/ABI 最终冻结。该阶段仍不创建正式标签、不发起
手动触发、不执行发布；未来正式发布需另外批准。

## 过渡和并发边界

- 新工作流不会追溯更改已经排队/运行的旧 SHA 工作流，也不会保护重跑旧运行
  或显式从未包含门禁的旧 ref 发起的手动触发。不能把“新 main 不触发”解释为
  “任何历史工作流都已被撤销”。合并窗口不应并发进行旧发布或旧工作流重跑。
- 远端标签复核与 GitHub 发布 API 写入不是原子事务。复核之后外部再次移动标签
  或删除标签仍有检查/使用间隙。固定 SHA 保证构建来源不漂移；`--verify-tag` 仅在
  CLI 检查时拒绝缺失标签，CLI 随后的发布 API 请求不带标签对象 ID 原子条件，
  因而不能保证检查后并发删除不会导致 API 重建标签。正式发布窗口必须避免修改/删除
  标签。这里不新增权限、标签保护设置或机密配置，也不声称提供了标签锁。
- 工作流的来源防护只验证发布来源和现有 ABI 声明，不能证明 2.0 API 已冻结、
  真实设备断电验证、最低内核兼容或完整发布验收已完成。

官方依据：

- [GitHub 工作流版本与触发规则](https://docs.github.com/en/actions/concepts/workflows-and-actions/workflows)
- [GitHub push 与 workflow_dispatch 事件](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows)
- [GitHub 旧运行重跑仍沿用原 SHA/ref](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs)
- [gh release create 与 --verify-tag](https://cli.github.com/manual/gh_release_create)
- [GitHub CLI 先检查标签、再创建发布的实现](https://github.com/cli/cli/blob/trunk/pkg/cmd/release/create/create.go)

## 可复核的本地测试

`tests/test_release_gate.py` 用 Python 3.8 标准库、Git 和 Bash，从当前工作流提取真实
shell；所有 Git 标签只存在于自动清理的本地临时测试样本，所有 `gh` 调用都被拒绝写入的
模拟实现替代。测试不会访问 GitHub，也不会运行发布工作流。

```sh
python3 tests/test_release_gate.py
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o gate-build --build_shared=n --build_tests=y \
  --build_private_tests=n --build_regression_tests=n
xmake test -g release-gate -j1
```

覆盖事件和输入拒绝、缺失/仅本地残留标签、非提交标签、版本/ABI/清单不匹配、
轻量/附注正确标签、手动触发分支隔离、远端标签删除/移动/同一提交重打、
构建 SHA 固定、首次发布和修复前的防护、任务依赖及唯一 `--verify-tag` 创建路径。
所有多行 shell 同时执行提取后的 `bash -n`。测试注册在现有 Xmake 测试集，发布工作流
变化也会触发现有 `core-validation` PR 检查。

YAML 语法可单独用 PyYAML（审查工具，不是项目运行依赖）检查：

```sh
python3 - <<'PY'
from pathlib import Path
import yaml
for name in ('release-publish', 'core-validation'):
    path = Path('.github/workflows/' + name + '.yml')
    data = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
    assert isinstance(data['on'], dict) and isinstance(data['jobs'], dict)
    print(path, 'YAML syntax OK')
PY
```

本地结构断言和 YAML 解析不等同于运行 GitHub Actions 调度器、认证或发布 API。真实发布
和线上合并后核验必须分别记录；不能为了验证门禁而试发正式标签或调用发布工作流。

## 补充：v2正式发布需要同源码的人为风险签收（#257）

在标签存在且版本/ABI匹配后，`prepare/gate`还必须实际执行
`python3 scripts/check_release_signoff.py --expected-version 2.0.0 --expected-tag v2.0.0`。
工作流从绑定的标签源码而非触发手动任务所在的分支读取该脚本和
`validation/release_signoff_v2.json`；后者当前十二项均为`pending`，
任何未签收项都阻断`authorized=true`。

签收先于**未来**正式标签操作另行审查和提交；冻结基线提交与标签
源码之间只允许签收文件变化，后续任何代码、测试、CI定义或包内容
改动都必须退回重新冻结与评审。原有的本地模拟标签/假`gh`负例保留，
新增带**明确夹具身份**的正例，不能把夹具中的假签收写回真实仓库。
本门禁是范围签收记录的机器一致性检查，**不验证GitHub评论的真实身份，
不改变远端权限设置**，正式发布前仍必须通过受保护分支/标签策略、
人工复核与正确的Release资产同源码验证。原标签绑定/移动复核/竞态
限制继续适用。
