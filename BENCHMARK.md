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
