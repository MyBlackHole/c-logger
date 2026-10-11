> 注意：历史性能数字不再作为有效发布基线。本轮修复丢失唤醒及输出完成口径；此前将所有超时归于 CPU 限流没有充分依据。新版 `bench_matrix` 等待实际输出水位，并使用实际输出数（emitted）而不是尝试数（attempted）计算有效端到端吞吐；本轮不据此宣称性能收益。

# 基准测试方法

`bench_matrix` 测量异步生产者路径以及从生产开始到输出尝试全部完成的耗时。默认后端为 `/dev/null`，因此该基准主要衡量
Logger 格式化、MPSC 竞争以及消费者/后端数据路径，而不是存储硬件性能。

测试矩阵：

- 生产者线程数：1、2、4、8、16、32、64
- 消息字节数：64、256、1024、4000
- 每个生产者默认记录数：10,000
- 队列容量：65,536

指标：

- 生产者吞吐：`attempted / producer_seconds`，即尝试写入记录数除以生产者完成时间
- 端到端吞吐：`emitted_records / end_to_end_seconds`，即实际输出记录数除以从生产开始到输出尝试全部完成的时间；不包含后续销毁耗时，也不表示持久化确认
- 入队与丢弃记录数
- 队列高水位
- 消费者处理记录数与批次数

当前运行入口（Xmake）：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build --build_shared=n \
  --build_tests=n --build_private_tests=n --build_regression_tests=n
xmake -j4 bench_matrix
BENCH_RECORDS=10000 scripts/benchmark.sh
```

过去的 `cmake --build build --target bench_matrix` 仅适用于仍含对应 CMake 构建描述的
历史提交，不是当前主线入口。当前测试配置见 [测试指南](TESTING.md)，
矩阵驱动脚本为 `scripts/benchmark.sh`；可用 `BUILD_DIR` 指定构建目录，
或用 `BENCH_BIN` 指定已构建的可执行文件。

`bench_matrix` 直接链接真实生产静态目标 `logger`；release 模式下库和基准驱动均使用
`-O3 -DNDEBUG`，生产库保持 `LOGGER_ENABLE_LOCKDEP=0`、故障注入关闭。
不需要启用测试支持库。该基准使用私有输出水位与队列布局，只在 `--build_shared=n`
配置提供，不能作为公开 API 或共享 DSO 性能验收。共享配置下不会回退到测试库。

`bench_matrix_accounting_regression` 保留原计数守恒 smoke，在启用
`build_regression_tests` 的静态/共享配置中均链接原回归支持库；它验证功能，不提供
生产性能数据。`bench_logger` 使用公开 API，静态/共享均直接链接生产目标，release
驱动也启用优化；其输出是生产者 attempted 速率，不是输出完成吞吐或持久化吞吐。

release 目录名本身不证明优化或链接口径。复核时保留 `xmake -rvD bench_matrix`
实际编译/链接命令、源码版本、编译器、配置与产物哈希。Debug/sanitizer 构建只作诊断。
本次构建修正前的 Xmake 基准链接了未优化且启用锁诊断的回归支持库，不可与修正后的
生产基准混作同口径数据，也不能把两者差值归因于某项生产源码优化。

结果以 CSV 和 JSONL 输出。性能门禁应比较来自同一机器、同一编译器、同一优化级别、同一 CPU 亲和性和同一后端的结果。
不要把不同主机上的数字直接判定为性能回归。

当前基准刻意不声明百分位延迟；准确的 P50/P95/P99 需要对每条记录进行时间戳采样并控制测量开销，
应作为独立基准实现。

## 时间戳缓存优化

`logger_format_line()` 会为每个线程缓存当前 `time_t` 秒对应的本地日期/时间和 UTC 偏移。
异步设计只有一个消费者，因此繁忙秒内几乎所有记录都会复用缓存。同步调用方各自使用独立的线程本地缓存，
不需要共享格式化锁。微秒以及所有逐记录元数据仍不缓存。

## 元数据快速格式化

启用时间戳缓存后，固定 Logger 元数据不再重复调用 `snprintf()`。有界追加辅助函数会直接写入字面量以及
整数 PID/TID/行号字段。用户提供的 printf 格式化语义保持不变，记录采集时仍使用 `vsnprintf()`。
这样既不会把 printf 语义引入自定义格式化器，又能降低消费者侧的元数据处理开销。

## 固定批大小 A/B 测试

消费者批大小分别以 64、128、256 和 512 条记录进行重复代表性负载测试。选择依据是相对批大小 64 的
端到端吞吐比几何平均值，而不是挑选单个有利场景。最终保留的固定批大小为 `256`。
自适应批处理暂缓，直到固定大小对比能够证明按负载切换值得引入额外复杂度。

## 固定到达率与真实文件（公开 API）

`bench_fixed_rate` 是独立测量入口，直接链接生产 `logger`，同时支持静态库与共享 DSO。
不会修改生产 API、开启测试钩子或把入队耗时称为落盘延迟。示例：

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build-rate --build_shared=n \
  --build_tests=n --build_private_tests=n --build_regression_tests=n
xmake -rvD bench_fixed_rate > fixed-rate-build.log 2>&1
python3 scripts/benchmark_fixed_rate.py \
  --binary "$(find build-rate -type f -name bench_fixed_rate -print -quit)" \
  --library "$(find build-rate -type f -name liblogger.a -print -quit)" \
  --build-log fixed-rate-build.log --out fixed-rate-results
```

共享配置将 `--build_shared` 改为 `y` 并指定实际 `liblogger.so.*`。应保存实际编译链接日志，
验证库与驱动均为 release 优化、无诊断/故障宏，不能只相信目录名。
驱动仅调用 `include/logger.h` 的公开符号；不包含内部头文件。

默认矩阵为 1/4 线程、64/1024 字节填充负载、每秒 10000/50000/100000 个计划调用、
1 秒预热 + 3 秒测量、`/dev/null`/真实临时文件、各 3 轮；奇数轮逆序降低顺序偏差。
输入为均匀绝对时刻 `start + slot / rate`，生产者按全局槽位轮流分担，不将每线程速率误作
总速率。迟到时仍尝试该槽位，可能出现追赶突发；到达窗口末尾后不再发新调用。
因此必须同时阅读迟到分位、超过 1ms 的次数和 `generator_missed`，不能把计划速率当成
已实现的平滑到达率。截止时正在进行的 API 调用仍等待返回并记录全部耗时，不截断尾部。
预热与测量在同一实例执行，预热先排空并刷新，再重建生产线程/预触测量数组。
这会产生短暂间隔，不是无间隙长稳态实验。队列容量为 65536，INFO 满队列策略 DROP，
不轮转、flush_level=OFF。`payload_bytes` 只计 `x` 填充，不含 phase/seq 标记和日志元数据。

指标口径：

- `api_ns`：每次公开异步日志调用从调用前至返回的 P50/P95/P99/最大耗时，包含时钟测量成本。
  包括被 DROP 的调用，公开 void API 不提供逐条接收结果。数组预触不在测量窗口内。
  `clock_pair_ns` 为 1000 次连续时钟对控制值，不能直接从分位相减。
- `arrival_late_ns`：实际调用开始相对原计划时刻的迟到分位，未发出槽位不纳入延迟分位。
  `scheduled = attempted + generator_missed`，发生器能力不足不能归为 Logger 丢弃。
- `attempted = enqueued + sync_fallbacks + dropped`；输出停止后还检查完成计数和
  `emitted + failed` 守恒。`valid` 只证明驱动/计数与 I/O 检查，不表示零丢弃、速率达标或业务 SLO。
- `output_observed_seconds`/`output_drain_observed_ns`：停止生产后以公开 I/O 完成计数
  轮询观测排空。1ms 轮询和调度误差使其成为观测上界；不是逐条输出延迟分位。
- `flush_ns`：排空之后调用一次真实文件 `logger_flush_instance_status()` 的整批 fsync
  屏障耗时。每阶段一次，不能叫每条日志的落盘 P99，也不证明物理断电安全。
  `/dev/null` 无 fsync 契约，跳过该测量，销毁只允许预期的 EINVAL。
- CPU 用户态/内核态秒数和 RSS 峰值来自独立子进程生命周期 `wait4`，包括初始化、预热、
  采样、排空、原始数组写出、分位排序及销毁，不能与仅测量窗口相除冒充业务 CPU 利用率。
  内存还包含测量数组，不能称为纯 Logger 内存。文件解析/压缩由父进程在测量之后执行。

原始数据：每次保留 stdout/stderr、退出状态、绝对 UTC 起止、计数/分位、文件系统类型、
资源数据、压缩后的完整日志及逐槽位延迟数组。`latency.bin.gz` 解压后为本机字节序 uint64：
每阶段两个值 phase/count，随后 count 个 API 纳秒值，再 count 个迟到纳秒值；重复各阶段。
API 值 0 表示该槽位未发出。runner 独立重算分位，并逐条验证真实文件的 phase/seq、
填充长度、无重复、无越界、必须属于尝试槽位、条数等于 emitted；删除/损坏/重复均失败。
机器信息、原始格式字节序、精确 SHA/tree/dirty diff、关键源码哈希、可执行文件与库哈希、
构建日志一并保存。共享/容器文件系统的 fsync 结果不等于专用物理磁盘吞吐。

资源限制：单样本总计划数不超过 200 万、测量不超过 30 秒、预热不超过 5 秒，预计输出
`count × (payload_bytes + 512)` 不超过 1GiB。外层默认 45 秒硬超时，超时杀死该样本进程组；
文件大小硬上限 1GiB、子进程地址空间上限 2GiB、禁 core。真实文件只位于 runner 自建的
临时目录，成功/失败均在保存可用证据后清理，可用 `--temp-root` 指定待测文件系统。
不直接传入业务日志路径。证据目录必须新建，保留证据超过 1GiB 时停止后续样本。
脚本最多 200 样本；用户需按磁盘空间和观察目标选择矩阵。SIGKILL 或主机崩溃仍可能留下临时目录；正常 Ctrl-C/SIGTERM 会先终止并回收样本子进程。

没有业务目标时，报告每个输入速率下的实际 attempted/emitted、drop、发生器缺失/迟到、
API 分位、整批 flush、资源和轮次范围；这是可重现的受限负载曲线，不编造通用 QPS 门槛。
达到最高测试速率且无丢弃也只能给出该环境/窗口的观察下界，不能称理论极限或长期容量。
