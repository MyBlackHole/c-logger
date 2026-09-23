> 注意：历史性能数字不再作为有效发布基线。本轮修复丢失唤醒及输出完成口径；此前将所有超时归于 CPU throttling 没有充分依据。新版 `bench_matrix` 等待实际输出水位，并使用 emitted 而不是 attempted 计算有效端到端吞吐；本轮不据此宣称性能收益。

# Benchmark methodology

`bench_matrix` measures the asynchronous producer path and complete drain time.
The default backend is `/dev/null` so the benchmark measures logger formatting,
MPSC contention and consumer/backend plumbing rather than storage hardware.

Matrix:

- producer threads: 1, 2, 4, 8, 16, 32, 64
- message bytes: 64, 256, 1024, 4000
- default records per producer: 10,000
- queue capacity: 65,536

Metrics:

- producer throughput: attempted records / producer completion time
- end-to-end throughput: attempted records / final drain+destroy time
- enqueued and dropped records
- queue high-water mark
- consumer records and batches

Run:

```sh
cmake --build build --target bench_matrix
BENCH_RECORDS=10000 scripts/benchmark.sh
```

Results are emitted as CSV and JSONL. Performance gates should compare results
from the same machine, compiler, optimization level, CPU affinity and backend.
Do not treat numbers from different hosts as regressions.

This benchmark intentionally does not claim percentile latency yet; accurate
P50/P95/P99 requires per-record timestamp sampling with controlled measurement
overhead and is a separate benchmark.

## Timestamp cache optimization

`logger_format_line()` caches the local-time date/time and UTC offset per thread
for the current `time_t` second. The async design has one consumer, so almost all
records in a busy second reuse the cache. Synchronous callers receive independent
thread-local caches and require no shared formatting lock. Microseconds and all
per-record metadata remain uncached.

## Metadata fast formatter

After timestamp caching, fixed logger metadata no longer uses repeated
`snprintf()` calls. Bounded append helpers write literals and integer PID/TID/
line fields directly. User-provided printf formatting is unchanged and still
uses `vsnprintf()` during record capture. This keeps printf semantics out of the
custom formatter while reducing consumer-side metadata overhead.

## Fixed batch-size A/B

The consumer batch was tested at 64, 128, 256 and 512 records using repeated
representative loads. Selection uses the geometric mean of end-to-end
throughput ratios versus batch 64, rather than choosing a single favorable
case. The retained fixed batch size is `256`. Adaptive batching is deferred
until a fixed-size comparison demonstrates that workload-dependent switching is
worth its complexity.
