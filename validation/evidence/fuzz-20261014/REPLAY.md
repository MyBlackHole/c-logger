# #211：干净恢复、工具链补证与历史语料重放

## 原件与补证

原始 `manifest.json`、`artifacts.tar.xz` 和 `verify.py` 保持不变。
`toolchain-supplement.json` 与 `pr208-toolchain-excerpt.txt` 从原始
Actions job 115866817217 补记 Clang 18.1.3 与实际 Xmake 3.1.1 构建横幅。
原 #208 ZIP 未包含 compiler.txt；补证是后取的日志节选，不冒称
原件内有该文件、完整工具链镜像已归档或已保存原始执行二进制。
节选 SHA-256 只标识节选本身，不等于完整历史 job 日志摘要。

## 原样恢复

现行校验器的实际参数为 `--restore`，不是 `--output-dir`：

```sh
python3 validation/evidence/fuzz-20261014/verify.py --restore /tmp/fuzz-originals-new
```

目标目录必须不存在。校验器先验证完整压缩包和原始 ZIP 的身份、
大小、成员路径/数量及 SOURCE_COMMIT，再恢复到新目录。
ZIP 成员位于 `<目标>/<artifact_id>/`，五份 ZIP 自身也保持原样。
`replay.py` 额外比较恢复出来的每个文件与解码原件的 SHA-256；
重放结束后再次校验，拒绝语料/日志被增加、删减或改写。

## 新构建上的历史输入重放

`.github/workflows/fuzz-archive-replay.yml` 是手动及路径限定的 PR/main
工作流，不是周期监控服务。每个作业单独干净检出本轮验证工具；从
原始清单取得 `tested_source_sha` 后，再干净检出历史测试合并提交。
源码必须精确匹配并且工作区干净，否则失败；不回退为最新 main。

五种模式分别对应原始五份制品：

| 模式 | 输入/断言 | 本轮构建 |
|---|---|---|
| public-fuzzer | 原 #208 归档 corpus 的每个文件，一次独立执行 | 历史生产源码的 Clang ASan/UBSan + 覆盖插桩测试归档 |
| format-fuzzer | 原 #209 归档 corpus 的每个文件，一次独立执行 | 同上，但使用原格式化目标 |
| format-plain | 原日志中的四个 seed 和 selftest | GCC 普通静态目标 |
| format-address-undefined | 同样的 seed/selftest，并比较原日志中的预期结果 | Clang ASan/UBSan |
| format-thread | 同样的 seed/selftest，并比较原日志中的预期结果 | GCC TSan |

这是对**归档输入的固定重放**，不继续生成新模糊输入。
每个 fuzzer 文件以普通文件参数传入，不把原 corpus 当作可变输出目录。
每次要求进程成功且输出真实运行标记；格式化模式还要求与原日志相同
的 seed、512 输入以及 selftest 的 3072 次负向拒绝。

从根目录获得某模式的精确历史源码：

```sh
python3 validation/evidence/fuzz-20261014/replay.py --mode public-fuzzer --print-source
```

在独立目录检出该提交、安装对应 GCC/Clang 与 Xmake 3.1.1 后执行：

```sh
XMAKE_CONFIGDIR=/tmp/c-logger-xmake-config \
python3 validation/evidence/fuzz-20261014/replay.py \
  --mode public-fuzzer --source-dir /tmp/c-logger-historical-source \
  --output-dir /tmp/c-logger-new-replay
```

Xmake 默认在项目内写本地配置；此处通过其 `XMAKE_CONFIGDIR` 环境变量
改放源码外，不修改历史 `.gitignore`，也不隐藏未跟踪的源码改动。
工作流为每种模式设置独立的临时配置目录，源码洁净断言保持严格。
新输出必须在源码目录之外且不能预先存在。操作系统和工具链按本轮
实际值记录；不要求与历史二进制哈希一致，也不自动下载执行归档内代码。
覆盖插桩归档不属于发布库，普通交付的 ELF/ABI/无测试插桩验证不变。

## 回执与验收边界

每个模式的 `receipt.json` 包含原制品身份、本轮工具提交、历史源码
提交/树、本轮工作流身份、实际编译器和 Xmake 输出、库/驱动新哈希、
所有恢复成员哈希、执行命令、退出状态、日志哈希及逐输入/种子结果。
失败或超时保留失败回执，不生成成功摘要；已有输出和日志不会覆盖。
恢复验证工具不使用“目录存在”或“零次运行成功”作为重放通过。

十项本地工具正反例验证缺失语料、路径符号链接、篡改、错误种子、
错误源码身份、非零退出、超时及旧日志拒绝覆盖。这些夹具不是 Logger
真实回归，实际验收只看上述五模式 Actions 的精确提交结果。

本轮 Actions 回执/日志保留 30 天；原件已经在 Git 长期保存。
结项时应将运行链接、每模式数量、工具链和新构建摘要写入 #211，
同步 #107/#100；不能把重放称为历史二进制恢复或全输入空间证明。
周期 Fuzz、覆盖趋势和新增失败最小化继续归 #212，发布范围收敛归 #210。
本工作不修改生产代码、ABI、版本，也不授权创建正式发布标签。
