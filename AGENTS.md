# c-logger 项目协作约定

- 面向使用者和维护者的文档、代码注释使用简体中文；程序输出通过国际化机制按语言呈现，不直接固定替换成中文。完整边界以 `docs/PROJECT_LANGUAGE.md` 为准；代码标识符、机器兼容面和第三方原文保留。
- 先阅读 `README.md`、`docs/CODING_STYLE.md`、`docs/v2/CODING_STYLE.md` 和 `TESTING.md`。本文件只补充语言约定，不改变现有架构或代码规范。
- 项目自身构建、测试、安装和打包以 Xmake 为准；CMake 保留为下游消费面。运行前从 `VERSION` 设置 `LOGGER_PROJECT_VERSION`。
- 区分 main 的 ABI 2 开发状态、release/1.x 与历史验证证据；翻译不得抹去版本和验证范围。
- 语言清理与行为修改分开提交。保留日志格式、状态码、导出符号、CI 身份与基准数据键；历史原始证据只补中文导读。
- 改动后按 `TESTING.md` 执行对应检查，PR 以中文列出实际通过、失败和未运行项。
