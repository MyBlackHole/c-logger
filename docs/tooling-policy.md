# 项目源码格式与分析工具

`.clang-format` 与 `.editorconfig` 定义项目源码格式；clang-tidy 不再另设 4 空格/Allman 风格。

## 新增和修改源码的格式门禁

`修改源码格式检查` 工作流固定使用 clang-format **18.1.8**，独立于日志文本
formatter 功能测试。它在普通 `pull_request`（包括 fork）和 main push 运行，只有
`contents: read` 权限，不使用 `pull_request_target`，不保留 checkout 凭据。

- 检出 PR 的实际 head SHA，并获取完整历史；比较 `merge-base(base SHA, head SHA)`
  到 head，不使用 GitHub 的临时 merge commit。push 使用事件的 before/head SHA。
  缺失、全零、无共同祖先或浅历史不足均报错，不能退回 HEAD 或假称通过。
- 覆盖 Git 已跟踪的 `.c/.h/.cc/.cpp/.cxx/.hh/.hpp/.hxx`，包括源码、头、测试、示例。
  新文件及从非 C-family 扩展名改名进入该范围的文件全检；修改文件只要求新侧
  新增/替换行符合格式。删除行、删除文件、纯改名和空 diff 无待检行，正常通过。
  NUL 分隔路径支持空格、引号、换行等；源码符号链接显式拒绝。
- clang-format 产生字节偏移 replacement，再与修改行相交；跨行空白同时涉及
  后一行缩进。可能需要整理与改动直接相邻的空白，但不要求清理无关历史文件。
  不写回文件、不全库重排、不以格式通过证明语义正确。模板 `.h.in` 不当成 C 头。
- 配置在空 diff 时也解析；版本不符、formatter 错误或 incomplete format 都失败。
  本地检查必须检出 head 且已提交修改，保证读取该版本的格式配置。新增忽略规则、
  `clang-format off` 或格式配置变更仍需人工审查，不能用来掩盖新增代码的格式问题。

本地复现（先提交修改，使用当前 PR 的目标分支引用或 base SHA）：

```sh
python3 -m venv /tmp/source-format-venv
/tmp/source-format-venv/bin/pip install 'clang-format==18.1.8'
export PATH="/tmp/source-format-venv/bin:$PATH"
python3 scripts/test_source_format.py
python3 scripts/check_source_format.py --base origin/main --head HEAD
```

自测在独立临时 Git 仓库覆盖合格/违规的新文件和修改行、历史债务保留、分叉后的
merge-base、空 diff、仅文档、删除、改名及特殊路径、UTF-8 字节偏移、无末尾换行、
缺失基准、脏工作区、错误版本和源码符号链接。它是门禁本身的测试，不是产品测试。
历史排版债务应使用独立纯格式 PR 渐进处理，不能与运行时修复混成整库格式化。

## clang-tidy 的实际覆盖边界

`.clang-tidy` 是供后续试运行的 **C 语言可选配置**，不是已接入的 CI 安全门。
`FormatStyle: file` 复用 `.clang-format`；仅保留 clang 编译诊断与 analyzer 的
core/unix/security 范围，移除 C++ modernize/google/performance 等宽泛选择；
项目头过滤范围明确为 include/src 下的 `.h`。WarningsAsErrors 暂为空，不把未经
建立基线的告警直接升为阻断。insecureAPI 家族沿用排除，须人工复核 API 契约。

本轮没有运行 clang-tidy，配置存在、格式门禁通过均不表示静态分析通过。
可靠接入前必须固定 analyzer 版本、生成真实 compile_commands.json、列明生产/测试
宏与平台组合、验证选中的检查及头文件覆盖，并为抑制项给出理由。禁止自动 `--fix`
制造大面积改动；格式工具也不证明线程安全、ownership、ABI 或所有故障路径正确。

工具人读帮助与诊断的语言选择、翻译目录维护及当前覆盖边界见
[工具输出国际化](tool-output-i18n.md)。
