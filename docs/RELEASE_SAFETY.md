# 显式发布门禁与安全迁移顺序

本变更只关闭 `main` 自动发布入口并验证显式发布来源；不修改 `VERSION`、包标题、
仓库权限或 secrets，也不代表批准任何正式 tag、手动 dispatch 或 release。

## 门禁合同

- `push` 只订阅 `v2.*` tag；普通 `main` push、`VERSION`、release notes 与本 workflow
  自身的合并都不是发布事件。`prepare.if` 另行拒绝 branch push、tag 删除及其他事件。
- 手动 `workflow_dispatch` 必须填写现存的 `v2.x.y` tag。这里的字符串输入不是自动创建
  tag 的请求；空值、branch、预发布后缀及非规范数字版本都失败关闭。
- 实际 shell 重新检查事件和格式，从 `origin` 获取精确 tag，再要求它可解析为 commit。
  tag push 的 commit 还必须与事件的 `GITHUB_SHA` 一致。dispatch 分支的 `VERSION`
  不参与发布：验证与构建读取选定 tag 的 commit。
- 选定 commit 必须具有相同 `VERSION`、ABI 2、`LOGGER_2.0`、配置的
  `abi/logger-2.0.symbols` 和非空符号条目。已有构建/产物检查仍负责验证实际 ELF、符号
  允许列表、SONAME、生产隔离与完整包内容；来源门禁不能代替它们。
- `prepare` 成功且产生门禁通过标记后，才允许下游执行。build 与 publish checkout
  固定 commit SHA，不使用可移动的 tag 名作为 checkout ref。build 前与第一次 release
  写操作前，都重新获取远端 tag，比较 commit SHA **及 tag 对象 ID**；因此同 commit
  的 annotated tag 被替换也会被拒绝。
- 只有一个 `gh release create` 路径，且必须使用 `--verify-tag`。不再存在缺失 tag 时
  回退到分支、`GITHUB_SHA` 或 `--target` 自动造 tag 的路径。已有 release 的修复同样
  经过来源复核。

## 必须遵循的迁移顺序

### A. 单独合并门禁

1. 先审查本 PR 和本地/PR 测试；保持 `VERSION` 和包标题不变。
2. 合并前记录 `main` SHA、现有 release 与资产基线，并读取仓库 Actions runs 的
   `queued`、`in_progress`、`waiting`、`pending` 状态，筛出 `release-publish`。
   若存在旧发布活动，暂停合并并报告；不要擅自取消发布或改变仓库权限。
3. 合并门禁 PR。GitHub 按事件关联的 commit/ref 读取 workflow，因而该合并产生的
   `main` push 使用**新 workflow**，不会根据前一版的 `main`/path 触发器发布。
4. 合并后确认远端 `main` 已包含门禁，查询该精确 merge SHA 对应的 workflow runs，
   确认没有 `release-publish` run；再次检查旧活动 run，并把 release/资产 ID、名称、
   大小、更新时间以及可用的 digest 与合并前基线比较，确认没有发布变化。
   这一步需实际 GitHub 只读证据，不能由本地测试替代。

### B. 另一个 PR 才迁移版本和标题

只有 A 的合并及线上无发布副作用核验完成后，才在另一个 PR 中迁移 `VERSION`、包标题
和相应文档。本 PR 不执行这一阶段。该阶段仍不创建正式 tag、不触发 dispatch、
不执行 release；未来正式发布需另外批准。

## 过渡和并发边界

- 新 workflow 不会追溯更改已经排队/运行的旧 SHA workflow，也不会保护重跑旧 run
  或显式从未包含门禁的旧 ref 发起的 dispatch。不能把“新 main 不触发”解释为
  “任何历史 workflow 都已被撤销”。合并窗口不应并发进行旧发布或旧 workflow 重跑。
- 远端 tag 复核与 GitHub release API 写入不是原子事务。复核之后外部再次移动 tag
  或删除 tag 仍有检查/使用间隙。固定 SHA 保证构建来源不漂移；`--verify-tag` 仅在
  CLI 检查时拒绝缺失 tag，CLI 随后的 release API 请求不带 tag 对象 ID 原子条件，
  因而不能保证检查后并发删除不会导致 API 重建 tag。正式发布窗口必须避免修改/删除
  tag。这里不新增权限、tag protection 设置或 secrets，也不声称提供了 tag 锁。
- workflow 的 source guard 只验证发布来源和现有 ABI 声明，不能证明 2.0 API 已冻结、
  真实设备断电验证、最低内核兼容或完整发布验收已完成。

官方依据：

- [GitHub workflow 版本与触发规则](https://docs.github.com/en/actions/concepts/workflows-and-actions/workflows)
- [GitHub push 与 workflow_dispatch 事件](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows)
- [GitHub 旧 run 重跑仍沿用原 SHA/ref](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs)
- [gh release create 与 --verify-tag](https://cli.github.com/manual/gh_release_create)
- [GitHub CLI 先检查 tag、再创建 release 的实现](https://github.com/cli/cli/blob/trunk/pkg/cmd/release/create/create.go)

## 可复核的本地测试

`tests/test_release_gate.py` 用 Python 3.8 标准库、Git 和 Bash，从当前 workflow 提取真实
shell；所有 Git tag 只存在于自动清理的本地临时 fixture，所有 `gh` 调用都被拒绝写入的
mock 替代。测试不会访问 GitHub，也不会运行发布 workflow。

```sh
python3 tests/test_release_gate.py
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o gate-build --build_shared=n --build_tests=y \
  --build_private_tests=n --build_regression_tests=n
xmake test -g release-gate -j1
```

覆盖事件和输入拒绝、缺失/仅本地残留 tag、非 commit tag、版本/ABI/manifest 不匹配、
lightweight/annotated 正确 tag、dispatch 分支隔离、远端 tag 删除/移动/同 commit 重打、
build SHA pin、首次发布和修复前的 guard、job 依赖及唯一 `--verify-tag` 创建路径。
所有多行 shell 同时执行提取后的 `bash -n`。测试注册在现有 Xmake suite，release workflow
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
和线上合并后核验必须分别记录；不能为了验证门禁而试发正式 tag 或调用发布 workflow。
