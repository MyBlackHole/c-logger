# Syslog 单调时刻与重连时长的有限类型隔离

本阶段实施 #106/#210 的 D1/T7 在 Syslog 重连路径上的有限子集。
现有时间换算和冷却算法没有在本轮审查中复现生产算术错误；本次改动增加
编译器可执行的单位边界，并为原有换算上界补充真实路径回归。

## 私有类型与换算边界

`src/logger_time.h` 定义两个不同的 C 结构体：

| 类型 | 表示和使用位置 |
|---|---|
| `logger_monotonic_ms` | `uint64_t` 单调毫秒时刻；Syslog 的取时输出、当前读数和重试起点 |
| `logger_duration_ms` | `uint32_t` 毫秒时长；Syslog 的重连间隔 |

公开 `logger_syslog_config_t.reconnect_interval_ms` 仍是原来的整数配置。
初始化先验证配置范围，再显式构造私有时长；零值仍采用 1000 ms，最大仍为
60000 ms。新头文件不安装，不增加公开 API、导出符号或新的超时接口。

`monotonic_ms` 仍读取 `CLOCK_MONOTONIC`，拒绝负秒、负纳秒和达到
1000000000 的纳秒值，返回 `-1` 并设置 `EIO`；秒转毫秒无法表示时返回
`EOVERFLOW`。只有全部检查成功后才写入时刻输出槽，纳秒仍向下取整到毫秒。

冷却比较使用同型时刻的先后关系，以及保持 64 位的经过时长差值。
`logger_monotonic_ms_elapsed` 先拒绝倒退，再比较 `now - since >= interval`，
不通过起点加时长构造期限。实际写入路径仍在倒退或断连取时失败后重建起点，
到期才重连；原有连接首错、最近错误、失败计数和不重放记录的行为保持。
这保持整数毫秒合同，不承诺从原始纳秒起点计满同样长的纳秒时间。

## 编译正反例

`tests/scripts/test_syslog_time_types.py` 必须取得 `LOGGER_TEST_CC`。
Xmake 的 `syslog_time_type_contract` 将真实回归目标选择的 C 编译器传入，
以 GNU C11、完整警告及 `-Werror` 执行 12 组正反编译对照：

- 裸整数、时刻和时长误赋给两个生产字段，或误传入比较函数；
- 真实私有 `monotonic_ms` 的输出槽误用时长指针或裸整数指针；
- 对真实 `logger_syslog.c` 分别仅替换经过时长比较的第三实参、重建窗口时的
  起点赋值，使时刻和时长混用。

每组先要求仅修正类型的正例成功，再要求反例在替换表达式的准确行出现类型
不兼容诊断。缺编译器、缺头文件、未知选项、信号和超时不能当作成功拒绝。
另外一个正例记录 C 仍允许的同型复制及显式提取 `.value` 后重新构造对象。
因此这些不同结构体防止隐式误混，不是完整的单位数据流或跨函数逃逸证明。

## 真实时间边界回归

`tests/regression/test_syslog_faults.c` 在测试侧包装时钟，输入原始 `timespec`，
使用真实 Logger、已登记 socket 和 Unix 数据报接收器；生产没有注入开关。
新增八个场景：

| 场景 | 精确断言 |
|---|---|
| `clock-negative-seconds` | 负秒拒绝为 `EIO` |
| `clock-negative-nanoseconds` | 负纳秒拒绝为 `EIO` |
| `clock-large-nanoseconds` | 纳秒达到 1000000000 拒绝为 `EIO` |
| `clock-max-milliseconds` | 18446744073709551 秒与 615999999 纳秒得到 `UINT64_MAX` 毫秒，并可真实发送 |
| `clock-millisecond-overflow` | 相同秒数与 616000000 纳秒拒绝为 `EOVERFLOW` |
| `clock-limit-cooldown` | 起点为 `UINT64_MAX - 100`，上界前 1 ms 仍冷却，恰到上界允许重连 |
| `clock-wide-elapsed` | 跨 `UINT32_MAX + 1` ms 允许重连，不将差值截为 32 位 |
| `clock-fractional-cooldown` | 向下取整后第 99 个整数毫秒仍冷却，第 100 个允许重连 |

四个非法读数场景分别检查公开创建的空拥有者槽、精确负错误码和调用方 errno
保留；直接初始化未发布起点且未获取 socket；存活实例不改原起点、不发起连接，
恢复后真实重连并保留创建阶段首错。成功和冷却场景检查实际系统调用及指标。
极值输入按当前 [Linux x86_64 支持矩阵](SUPPORT_MATRIX.md) 检查 `time_t`
宽度和值往返，不能静默截断或跳过后计作通过。

## Xmake 入口与验收范围

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release --build_shared=n --build_regression_tests=y
xmake test -g syslog-time -j1
```

静态配置共注册 25 项：上述八个场景与原有 `clock-init`、`deferred-cooldown`、
`backwards-clock`、`clock-disconnect`，各自在回归支持库和真实生产静态归档
执行一次，另加一项编译契约。生产归档复用既有 `syslog_candidate_production`
目标，同一目标中的 fd 所有权测试仍属于原分组。

共享配置注册支持库的 12 个时间场景和编译契约，不声称 GNU `--wrap` 拦截了
DSO 内部调用；真实共享后端、rsyslogd、安装消费、最低工具链及 Sanitizer
继续由适用工作流分别验证。具体通过数以最终提交的原始执行结果为准。

本阶段没有迁移文件墙钟时间、所有整数单位或其他期限接口，也不自动完成
D1/T7 的全部对象、D1–D6 签收或 2.0 正式发布。运行回归证明列出的实际路径；
编译反例证明误混被拒绝，不能据此宣称修复了此前未复现的生产溢出。
