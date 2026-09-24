#!/usr/bin/env python3
import argparse
import json
import statistics
import subprocess
from pathlib import Path


def parse_csv_ints(value):
    return [int(x) for x in value.split(",") if x]


def run_one(binary, threads, records, message_bytes):
    proc = subprocess.run(
        [binary, str(threads), str(records), str(message_bytes)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    lines = [line for line in proc.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError(f"{binary}: benchmark produced no JSON output")
    result = json.loads(lines[-1])
    if not result.get("valid", False):
        raise RuntimeError(
            f"{binary}: invalid accounting for threads={threads}, "
            f"bytes={message_bytes}: {result}"
        )
    return result


def median(samples, key):
    return statistics.median(float(x.get(key, 0)) for x in samples)


def any_nonzero(samples, key):
    return any(int(x.get(key, 0)) != 0 for x in samples)


def mib(value):
    return value / (1024.0 * 1024.0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-bin", required=True)
    parser.add_argument("--candidate-bin", required=True)
    parser.add_argument("--out-dir", default="benchmark-comparison")
    parser.add_argument("--threads", default="1,4,16")
    parser.add_argument("--sizes", default="64,256,1024,4000")
    parser.add_argument("--records", type=int, default=1000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--default-queue", type=int, default=8192)
    parser.add_argument("--min-memory-reduction", type=float, default=50.0)
    parser.add_argument(
        "--baseline-ref",
        default="23504f104e900419e891b57d7187ca0bcabffdf3",
    )
    args = parser.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # 用一次极小运行读取真实编译器布局；不拿这次吞吐数据参与性能比较。
    baseline_layout = run_one(args.baseline_bin, 1, 1, 64)
    candidate_layout = run_one(args.candidate_bin, 1, 1, 64)

    baseline_slot = int(baseline_layout["queue_slot_bytes"])
    candidate_slot = int(candidate_layout["queue_slot_bytes"])
    spill_cap = int(candidate_layout.get("queue_spill_capacity", 0))
    spill_storage = int(candidate_layout.get("queue_spill_storage_bytes", 0))
    spill_block = spill_storage // spill_cap if spill_cap else 0
    default_spill_cap = min(args.default_queue, spill_cap)
    baseline_default = baseline_slot * args.default_queue
    candidate_default = (
        candidate_slot * args.default_queue + spill_block * default_spill_cap
    )
    reduction = (
        100.0 * (baseline_default - candidate_default) / baseline_default
        if baseline_default
        else 0.0
    )

    memory = {
        "baseline_ref": args.baseline_ref,
        "default_queue_capacity": args.default_queue,
        "baseline_slot_bytes": baseline_slot,
        "candidate_slot_bytes": candidate_slot,
        "candidate_spill_block_bytes": spill_block,
        "candidate_spill_capacity_at_default": default_spill_cap,
        "baseline_default_queue_bytes": baseline_default,
        "candidate_default_queue_bytes": candidate_default,
        "memory_reduction_percent": reduction,
        "minimum_required_reduction_percent": args.min_memory_reduction,
        "gate_passed": reduction >= args.min_memory_reduction,
    }

    rows = []
    raw = []
    for threads in parse_csv_ints(args.threads):
        for size in parse_csv_ints(args.sizes):
            base_samples = []
            cand_samples = []
            for repeat in range(args.repeats):
                # 交替运行顺序，降低 runner 温度/频率漂移的单向偏差。
                if repeat % 2 == 0:
                    base_samples.append(
                        run_one(args.baseline_bin, threads, args.records, size)
                    )
                    cand_samples.append(
                        run_one(args.candidate_bin, threads, args.records, size)
                    )
                else:
                    cand_samples.append(
                        run_one(args.candidate_bin, threads, args.records, size)
                    )
                    base_samples.append(
                        run_one(args.baseline_bin, threads, args.records, size)
                    )

            base_prod = median(base_samples, "producer_logs_per_sec")
            cand_prod = median(cand_samples, "producer_logs_per_sec")
            base_e2e = median(base_samples, "end_to_end_logs_per_sec")
            cand_e2e = median(cand_samples, "end_to_end_logs_per_sec")

            overloaded = (
                any_nonzero(base_samples, "dropped")
                or any_nonzero(base_samples, "sync_fallbacks")
                or any_nonzero(cand_samples, "dropped")
                or any_nonzero(cand_samples, "sync_fallbacks")
                or any_nonzero(cand_samples, "queue_spill_exhaustions")
            )
            comparable = not overloaded
            rows.append(
                {
                    "threads": threads,
                    "message_bytes": size,
                    "baseline_producer_logs_per_sec": base_prod,
                    "candidate_producer_logs_per_sec": cand_prod,
                    "producer_ratio": cand_prod / base_prod if comparable and base_prod else None,
                    "baseline_end_to_end_logs_per_sec": base_e2e,
                    "candidate_end_to_end_logs_per_sec": cand_e2e,
                    "end_to_end_ratio": cand_e2e / base_e2e if comparable and base_e2e else None,
                    "candidate_median_dropped": median(cand_samples, "dropped"),
                    "candidate_median_sync_fallbacks": median(
                        cand_samples, "sync_fallbacks"
                    ),
                    "candidate_median_spill_exhaustions": median(
                        cand_samples, "queue_spill_exhaustions"
                    ),
                    "throughput_comparable": comparable,
                }
            )
            raw.append(
                {
                    "threads": threads,
                    "message_bytes": size,
                    "baseline": base_samples,
                    "candidate": cand_samples,
                }
            )

    result = {
        "memory": memory,
        "benchmark": {
            "records_per_thread": args.records,
            "repeats": args.repeats,
            "threads": parse_csv_ints(args.threads),
            "message_bytes": parse_csv_ints(args.sizes),
            "throughput_is_informational_only": True,
            "rows": rows,
        },
        "raw_samples": raw,
    }
    (out / "results.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    md = []
    md.append("# Queue benchmark before/after")
    md.append("")
    md.append(
        f"基线 commit: `{args.baseline_ref}`；每个组合 {args.repeats} 次，"
        f"每线程 {args.records} 条。吞吐结果只用于观察，不作为 CI 性能门禁。"
    )
    md.append("")
    md.append("## 默认 queue 内存门禁")
    md.append("")
    md.append("| 指标 | baseline | candidate |")
    md.append("|---|---:|---:|")
    md.append(
        f"| slot bytes | {baseline_slot} | {candidate_slot} |"
    )
    md.append(
        f"| queue capacity={args.default_queue} 预分配 | "
        f"{baseline_default} B ({mib(baseline_default):.2f} MiB) | "
        f"{candidate_default} B ({mib(candidate_default):.2f} MiB) |"
    )
    md.append(
        f"| candidate spill | - | {default_spill_cap} blocks × {spill_block} B |"
    )
    md.append("")
    md.append(
        f"内存下降 **{reduction:.2f}%**；门禁要求 >= "
        f"**{args.min_memory_reduction:.2f}%**："
        f"{'PASS' if memory['gate_passed'] else 'FAIL'}。"
    )
    md.append("")
    md.append("## 吞吐对比")
    md.append("")
    md.append(
        "只有 baseline/candidate 都没有 drop、sync fallback、spill exhaustion 的组合"
        "才计算 ratio；否则标记 overload，避免把丢日志误当成性能提升。"
    )
    md.append("")
    md.append(
        "| threads | bytes | base prod/s | new prod/s | prod ratio | "
        "base e2e/s | new e2e/s | e2e ratio | drop | fallback | spill exhaust |"
    )
    md.append("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        prod_ratio = (
            f"{row['producer_ratio']:.3f}x"
            if row["producer_ratio"] is not None
            else "overload"
        )
        e2e_ratio = (
            f"{row['end_to_end_ratio']:.3f}x"
            if row["end_to_end_ratio"] is not None
            else "overload"
        )
        md.append(
            f"| {row['threads']} | {row['message_bytes']} | "
            f"{row['baseline_producer_logs_per_sec']:.0f} | "
            f"{row['candidate_producer_logs_per_sec']:.0f} | {prod_ratio} | "
            f"{row['baseline_end_to_end_logs_per_sec']:.0f} | "
            f"{row['candidate_end_to_end_logs_per_sec']:.0f} | {e2e_ratio} | "
            f"{row['candidate_median_dropped']:.0f} | "
            f"{row['candidate_median_sync_fallbacks']:.0f} | "
            f"{row['candidate_median_spill_exhaustions']:.0f} |"
        )

    summary = "\n".join(md) + "\n"
    (out / "summary.md").write_text(summary, encoding="utf-8")
    print(summary)

    if not memory["gate_passed"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
