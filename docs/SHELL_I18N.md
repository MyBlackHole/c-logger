# Shell 开发脚本国际化

本批仅覆盖 `scripts/check.sh` 与 `scripts/benchmark.sh` 自有的五条人读提示。
CSV 表头、JSONL、参数名、构建命令、退出码以及 stdout/stderr 归属保持不变。
Xmake/CMake 本身及构建选项帮助、编译器诊断、安装元数据不在本批范围。
普通 C 库使用者不需要安装 gettext，也不增加 `libintl` 链接依赖。

## 使用

英文原文始终可用。中文需要 GNU gettext 运行时的 `gettext` 与 `envsubst` 命令
（Debian/Ubuntu 的 `gettext-base` 包），以及已安装的系统 UTF-8 locale：

```sh
LC_ALL=zh_CN.UTF-8 scripts/check.sh fast
LC_ALL=en_US.UTF-8 LANGUAGE=zh_CN scripts/check.sh fast
LC_ALL=C scripts/check.sh fast
```

由 GNU gettext 按标准 `LC_ALL`、`LC_MESSAGES`、`LANG` 和 `LANGUAGE` 解析语言。
`C`、`POSIX` 和 `C.UTF-8` 使用英文；`LANGUAGE` 可列出以冒号分隔的回退语言。
系统不支持所选 locale 时可能回退英文，请先用 `locale -a` 检查。
Shell 不新增命令行语言参数，也不读取 Python 工具的 `CLOGGER_LANG`。
语言设置同样会被调用的第三方工具继承；本脚本不改变第三方工具的输出策略。

缺少运行时工具、缺少目录中的 MO 或找不到译文时，保持英文原文。
缺依赖时不向机器输出流额外打印警告。MO 路径由脚本自身位置确定，不依赖当前目录。

## 翻译与验证

英文完整消息是 msgid，中文位于
`locales/zh_CN/LC_MESSAGES/c-logger-shell.po`。
开发者使用 GNU gettext 工具包（`gettext` 包）更新模板、校验并编译：

```sh
xgettext --language=Shell --from-code=UTF-8 --keyword=logger_message:1 \
  --flag=logger_message:1:sh-format --package-name=c-logger-shell \
  --msgid-bugs-address=https://github.com/MyBlackHole/c-logger/issues \
  -o locales/c-logger-shell.pot scripts/check.sh scripts/benchmark.sh
msgmerge --update locales/zh_CN/LC_MESSAGES/c-logger-shell.po locales/c-logger-shell.pot
msgfmt --check --check-format --no-hash \
  -o locales/zh_CN/LC_MESSAGES/c-logger-shell.mo \
  locales/zh_CN/LC_MESSAGES/c-logger-shell.po
python3 tests/scripts/test_shell_i18n.py -v
```

提交 PO 和确定性编译出的 MO，运行脚本无需即时编译。
占位符采用命名变量 `$profile`、`$csv`、`$json`；翻译可调换次序，不能删除或新增变量。
`sh-format` 与 `msgfmt --check-format` 拒绝占位符不匹配。
`envsubst` 只替换原文声明的变量，不执行译文中的命令替换，也不使用 `eval`。
缺少工具时，调用处提供的已展开英文消息作为回退。

完整测试需要 GNU gettext、envsubst、msgfmt，以及 `zh_CN.UTF-8`、`en_US.UTF-8`
系统 locale。未安装 gettext 运行时的环境仍执行英文/机器输出测试，双语部分明确标为跳过。
独立 CI 工作流 `shell-i18n.yml` 安装 GNU 工具、通过 `localedef --no-archive` 创建
私有 `LOCPATH`，并用 `--require-gettext` 强制检查依赖和 UTF-8 locale；缺项直接失败，
不允许双语测试静默跳过。本测试无需加入 Xmake 聚合测试。

参考：[GNU gettext shell 支持](https://www.gnu.org/software/gettext/manual/html_node/sh.html)、
[envsubst 安全替换边界](https://www.gnu.org/software/gettext/manual/html_node/envsubst-Invocation.html)。
