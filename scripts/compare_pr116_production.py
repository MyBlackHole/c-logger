#!/usr/bin/env python3
"""在同一运行器上交替比较 PR #116 的前后生产库，性能数据只作观察。"""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

BEFORE = "c694c1220ea0df46b15e2cd72d67a610ca262e3f"
AFTER = "61b619d2461843b0bc9e49c5b4d324cead9da774"
FIELDS = (
    "attempted", "enqueued", "emitted", "failed", "dropped",
    "sync_fallbacks", "spill_exhaustions", "queue_storage_bytes",
    "producer_wake_signals", "worker_wait_count", "api_p50_ns",
    "api_p95_ns", "api_p99_ns", "producer_seconds",
    "end_to_end_seconds", "producer_logs_per_sec",
    "end_to_end_logs_per_sec", "rss_peak_kib",
    "user_cpu_sec", "system_cpu_sec",
)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as src:
        for block in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cases(records, pressure_records):
    return [
        ("常规/1线程/64字节", 1, 64, 65536, records),
        ("常规/4线程/64字节", 4, 64, 65536, records),
        ("常规/1线程/1024字节", 1, 1024, 65536, records),
        ("常规/4线程/1024字节", 4, 1024, 65536, records),
        ("高压/4线程/64字节", 4, 64, 1024, pressure_records),
        ("高压/4线程/1024字节", 4, 1024, 1024, pressure_records),
    ]


def validate_sample(row, case):
    _, threads, size, capacity, records = case
    expected = threads * records
    if not isinstance(row, dict) or row.get("valid") is not True:
        raise ValueError("invalid_accounting")
    for key in FIELDS:
        value = row.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("missing_or_invalid_" + key)
        if value < 0:
            raise ValueError("negative_" + key)
    for key, value in (
        ("threads", threads), ("records_per_thread", records),
        ("payload_bytes", size), ("queue_capacity", capacity),
        ("attempted", expected),
    ):
        if row.get(key) != value:
            raise ValueError("workload_mismatch_" + key)
    if (row["enqueued"] + row["dropped"] +
            row["sync_fallbacks"] != expected):
        raise ValueError("enqueue_accounting_mismatch")
    if row["emitted"] + row["failed"] != (
            row["enqueued"] + row["sync_fallbacks"]):
        raise ValueError("output_accounting_mismatch")
    if row["failed"] != 0 or row["queue_storage_bytes"] == 0:
        raise ValueError("output_failure_or_missing_queue_memory")
    if not (0 < row["api_p50_ns"] <= row["api_p95_ns"] <=
            row["api_p99_ns"]):
        raise ValueError("invalid_latency_percentiles")
    if row["producer_seconds"] <= 0 or row["end_to_end_seconds"] <= 0:
        raise ValueError("invalid_duration")
    return row


def clean_pair(before, after):
    """仅两侧都没有丢弃/同步回退/输出失败时才比较有效吞吐率。"""
    for row in (before, after):
        if any(row[key] for key in ("dropped", "sync_fallbacks", "failed")):
            return False
    return True


def one(binary, case, timeout):
    _, threads, size, capacity, records = case
    result = subprocess.run(
        [str(binary), str(threads), str(records), str(size), str(capacity)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, check=False,
    )
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if result.returncode or len(lines) != 1:
        raise RuntimeError(
            "benchmark_failed: rc={} stdout={} stderr={}".format(
                result.returncode, result.stdout[-1500:],
                result.stderr[-1500:]))
    return validate_sample(json.loads(lines[0]), case)


def median(rows, key):
    return statistics.median(float(row[key]) for row in rows)


def value_range(rows, key):
    values = [row[key] for row in rows]
    return "{}–{}".format(min(values), max(values))


def summarize(rows, case_names, metadata):
    header = [
        "# PR #116 生产等价直接前后对照",
        "",
        "- 前版本：" + BEFORE,
        "- 后版本：" + AFTER,
        "- 驱动及实际生产库 SHA-256 见 metadata.json；两侧的构建输入是各自历史提交。",
        "- 同一运行器、相同编译器/优化选项、生产静态库直连、相同 CPU 亲和性；按轮次交替顺序。",
        "- **这是 1.0 历史 PR #116 的改动成本对照，不证明当前 2.0 主线无性能退化。**",
        "- **不设置共享运行器上的主观吞吐百分比硬阈值；计数、源码/构建身份和脚本失败为硬门禁。**",
        "- API 分位包含格式化、队列写入以及被 DROP 的调用；不是落盘延迟。",
        "- 发生丢弃或同步回退的轮次只报告原值，不将实际输出吞吐比解释为同负载性能增益。",
        "",
        "| 场景 | 前版有效输出/秒（范围） | 后版有效输出/秒（范围） | "
        "前版 API P99 纳秒（范围） | 后版 API P99 纳秒（范围） | "
        "前版 DROP（范围） | 后版 DROP（范围） | "
        "可比较配对轮数 | 无损配对吞吐后/前中位数 |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for case_name in case_names:
        records = [r for r in rows if r["case"] == case_name and not r["warmup"]]
        before = [r for r in records if r["variant"] == "before"]
        after = [r for r in records if r["variant"] == "after"]
        if len(before) != metadata["repeats"] or len(after) != metadata["repeats"]:
            raise ValueError("missing_paired_samples")
        pairs = []
        for iteration in range(metadata["repeats"]):
            b = next(r for r in before if r["round"] == iteration)
            a = next(r for r in after if r["round"] == iteration)
            if b["queue_storage_bytes"] != a["queue_storage_bytes"]:
                raise ValueError("queue_memory_layout_changed")
            if clean_pair(b, a):
                pairs.append(a["end_to_end_logs_per_sec"] /
                             b["end_to_end_logs_per_sec"])
        ratio = "{:.4f}".format(statistics.median(pairs)) if pairs else "不可比较"
        header.append(
            "| {} | {} | {} | {} | {} | {} | {} | {}/{} | {} |".format(
                case_name, value_range(before, "end_to_end_logs_per_sec"),
                value_range(after, "end_to_end_logs_per_sec"),
                value_range(before, "api_p99_ns"),
                value_range(after, "api_p99_ns"),
                value_range(before, "dropped"),
                value_range(after, "dropped"),
                len(pairs), metadata["repeats"], ratio))
    header.extend([
        "",
        "原始 trials.jsonl 包含所有轮次的生产者吞吐、端到端输出吞吐、"
        "P50/P95/P99、丢弃、同步回退、spill 耗尽、通知数、"
        "固定队列内存、峰值 RSS 与 CPU；不把进程 RSS 当作纯 Logger 内存。",
        "",
        "历史 queue-benchmark 的固定早期吞吐基线属于趋势参考，"
        "不是 PR #116 的直接父版本，也不参与以上比值计算。",
        "",
    ])
    return "\n".join(header)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before-bin", type=Path, required=True)
    parser.add_argument("--after-bin", type=Path, required=True)
    parser.add_argument("--before-lib", type=Path, required=True)
    parser.add_argument("--after-lib", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=4)
    parser.add_argument("--records", type=int, default=2000)
    parser.add_argument("--pressure-records", type=int, default=10000)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()

    if not 2 <= args.repeats <= 10 or not 1 <= args.records <= 20000:
        parser.error("invalid repeats/records")
    if not 1 <= args.pressure_records <= 20000 or not 5 <= args.timeout <= 120:
        parser.error("invalid pressure-records/timeout")
    binaries = {"before": args.before_bin.resolve(),
                "after": args.after_bin.resolve()}
    libraries = {"before": args.before_lib.resolve(),
                 "after": args.after_lib.resolve()}
    args.out.mkdir(parents=True, exist_ok=False)
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    meta = {
        "before_source": BEFORE, "after_source": AFTER,
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "machine": platform.machine(),
        "processor": platform.processor(), "cpu_affinity": affinity,
        "cpu_count": os.cpu_count(), "repeats": args.repeats,
        "records": args.records, "pressure_records": args.pressure_records,
        "binaries_sha256": {k: sha256(v) for k, v in binaries.items()},
        "production_libraries_sha256": {k: sha256(v) for k, v in libraries.items()},
    }
    (args.out / "metadata.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2) + "\n")
    rows = []
    workloads = cases(args.records, args.pressure_records)
    case_names = [item[0] for item in workloads]
    try:
        # 不把预热结果混入正式分位，每侧预热独立新进程。
        for variant in ("before", "after"):
            row = dict(one(binaries[variant],
                           ("预热", 1, 64, 65536, min(args.records, 500)),
                           args.timeout),
                       case="预热", variant=variant, round=-1, warmup=True)
            rows.append(row)
        with (args.out / "trials.jsonl").open("w") as raw:
            for row in rows:
                raw.write(json.dumps(row, ensure_ascii=False) + "\n")
                raw.flush()
            for case in workloads:
                for iteration in range(args.repeats):
                    order = ("before", "after") if iteration % 2 == 0 else (
                        "after", "before")
                    for variant in order:
                        result = one(binaries[variant], case, args.timeout)
                        row = dict(result, case=case[0], variant=variant,
                                   round=iteration, warmup=False)
                        rows.append(row)
                        raw.write(json.dumps(row, ensure_ascii=False) + "\n")
                        raw.flush()
        summary = summarize(rows, case_names, meta)
        (args.out / "summary.md").write_text(summary)
        with (args.out / "trials.csv").open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(summary)
        return 0
    except Exception as exc:
        (args.out / "error.txt").write_text(
            "{}: {}\n".format(type(exc).__name__, str(exc)))
        raise


if __name__ == "__main__":
    sys.exit(main())
