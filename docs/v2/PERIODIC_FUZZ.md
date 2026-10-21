# #212 周期 fuzz 运行与故障处理

`periodic-fuzz` 每周一 02:17 UTC 在 GitHub Actions 运行。两个矩阵任务分别
编译 `public_fuzz.c` 和 `format_fuzz.c`，链接同一提交、带 ASan、UBSan 和
libFuzzer 覆盖插桩的生产静态库。PR 与 main 推送只运行运行器契约测试；
手动运行可选择 `normal` 或 `drill`。

每个正常入口探索 120 秒，进程 CPU 硬限额 130 秒、外部墙钟限额 150 秒；
单输入超时 10 秒、RSS 限额 2048 MiB、输入长度上限 128 字节，整个矩阵任务
限时 15 分钟。seed 由运行 ID 和尝试次数计算并写入 `result.json`。
运行器把 `TMPDIR` 定向到证据目录；测试临时目录也位于工作区的 `build/`。

每次运行记录源提交和 tree、编译器与 Xmake 版本、静态库与入口哈希、运行参数、
原始输出及按 SHA-256 去重的语料。制品在 Actions 保留 30 天。一次绿色运行只
证明这一组源码、工具链、语料和预算下没有发现故障，不代表全输入空间通过。
每次成功运行还在 #212 留下源码、产物、seed、参数、语料数量和结果摘要，
使关键来源在临时制品到期后仍可核对。

失败时，运行器保留原始输入并尝试最小化和重放。工作流先上传制品，再以
`result.json` 的状态、样本哈希、源码提交和运行链接创建 Issue，最后使任务保持
失败。下载制品后，先检查 `run.log`、`minimize.log`、`replay.log` 和
`result.json`，使用记录的源码和工具链重放 `minimized.input`；最小化不可用时
重放 `raw.input`。定位后在修复 PR 中加入回归输入，并把它纳入对应的固定种子
语料；用修复后的同一入口重跑，再在 Issue 中关联 PR、验证记录和语料变更后关闭。
仅换 seed 或删除输入不能作为修复。若未产生样本，保留日志并调查运行器或资源
限制，Issue 标记为 `failure_no_sample`。

`drill` 使用现有的 UBSan 整数溢出探针，检查捕获、最小化和重放链。
手动运行时启用 `register_drill_issue` 会创建并关闭一个明确标为演练的 Issue，
用来验证 GitHub 登记权限；它不表示 Logger 产品缺陷。演练制品同样保留 30 天。

本地运行契约测试时先创建工作区目录并设置 `TMPDIR`：

```sh
mkdir -p build/periodic-fuzz-tmp
TMPDIR="$PWD/build/periodic-fuzz-tmp" PYTHONDONTWRITEBYTECODE=1 \
  python3 tests/scripts/test_periodic_fuzz_runner.py -v
TMPDIR="$PWD/build/periodic-fuzz-tmp" PYTHONDONTWRITEBYTECODE=1 \
  python3 tests/scripts/test_register_periodic_fuzz_failure.py -v
```
