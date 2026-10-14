# 格式化模型、容量边界与校验器负向测试（#107）

`format_fuzz.c` 使用真正创建的同步 Logger，经私有格式化入口调用
当前生产源码编译的静态归档。它不构造假的 logger_t，也不调用库内
basename、level_name、格式化或截断辅助函数生成期望值。
这些私有符号并非 DSO 公开接口，因此不宣称共享库用户接口已被本测试覆盖。

## 独立期望与受界输入

模型使用三组固定 UTC 日期（含闰日和 2038 秒边界）、微秒位数、
八种级别显示、四种详细度、pid/tid/来源开关及有界上下文/正文，
生成完整期望文本。输入均映射为有效指针和 NUL 结尾字段，
不会将任意模糊字节当作 printf 格式串或悬空指针。

每次输入选择零/一/二字节、恰好不足、恰好足够、额外一字节、
随机容量或标准行容量之一。严格入口逐项检查返回码、错误时零长度
及首字节清空、足够空间时的精确文本；普通入口校验换行、NUL 和
截断标记。两侧各 16 字节哨兵必须完整。非法纳秒分别为 -1 和
1000000000；最后恢复有效纳秒并重测完整输出，检查失败后缓存复用。

四个固定 seed 各生成 512 个输入。selftest 对 512 个真实有效结果
分别制造错误状态、错误长度、换行损坏、NUL 损坏和前后边界破坏，
要求 **3072/3072 被校验器拒绝**。这是对校验器错误识别能力的验证，
不是通过编译期变异生产库得到的全程序变异测试分数。

## 运行与证据身份

Xmake 仍是被测 logger 目标的构建入口；测试脚本沿用项目现有的
生产静态归档重链接模式，不新增生产代码、宏或公开符号：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m debug -o format-build --build_shared=n
xmake -j2 logger
bash tests/regression/run_format_properties.sh format-build plain
```

独立工作流 `format-fuzz-properties` 对普通 GCC、Clang ASan/UBSan、
GCC TSan 和 Clang 原生 libFuzzer 分别构建及验证。fuzzer 构建只在
测试归档启用 `inline-8bit-counters,pc-table,trace-cmp`，并检查库归档
自身存在覆盖回调。交付库仍由原发布工作流单独构建和检查，不能把
插桩测试归档称为原样交付产物。所有运行时检查设为出错立即失败。

八个可读初始样本位于 `format-corpus/`；fuzzer 以 seed=209、3000 次
有界运行回放和变异，保留语料、原始结果、SOURCE_COMMIT、编译器及
库/驱动 SHA-256。普通和运行时检查模式运行四个 seed 及 selftest。
本地手动编译仅为预检，不能替代最终 PR head 的 Xmake/Actions 门禁。

## 不做的推论

该模型不覆盖任意时区/区域设置、printf 全语法、未经 NUL 终止的
非法对象、所有线程交错或后端落盘。现有时钟/编码故障回归及安装
消费验证仍分别承担对应合同。有限 fuzz 无崩溃不构成完整安全证明，
#107 和正式 v2.0.0 发布授权仍独立验收。
