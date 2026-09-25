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
    parser.add_argument("--memory-baseline-bin", required=True)
    parser.add_argument("--throughput-baseline-bin", required=True)
    parser.add_argument("--candidate-bin", required=True)
    parser.add_argument("--out-dir", default="benchmark-comparison")
    parser.add_argument("--threads", default="1,4,16")
    parser.add_argument("--sizes", default="64,256,1024,4000")
    parser.add_argument("--records", type=int, default=1000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--default-queue", type=int, default=8192)
    parser.add_argument("--min-memory-reduction", type=float, default=50.0)
    parser.add_argument("--memory-baseline-ref", required=True)
    parser.add_argument("--throughput-baseline-ref", required=True)
    args = parser.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # layout 读取只用于确定性内存门禁，不参与吞吐 median。
    memory_layout = run_one(args.memory_baseline_bin, 1, 1, 64)
    tuning_layout = run_one(args.throughput_baseline_bin, 1, 1, 64)
    candidate_layout = run_one(args.candidate_bin, 1, 1, 64)

    memory_slot = int(memory_layout["queue_slot_bytes"])
    tuning_slot = int(tuning_layout["queue_slot_bytes"])
    candidate_slot = int(candidate_layout["queue_slot_bytes"])
    tuning_spill_cap = int(tuning_layout.get("queue_spill_capacity", 0))
    tuning_spill_storage = int(
        tuning_layout.get("queue_spill_storage_bytes", 0)
    )
    tuning_spill_block = (
        tuning_spill_storage // tuning_spill_cap if tuning_spill_cap else 0
    )
    tuning_default_spill_cap = min(args.default_queue, tuning_spill_cap)

    spill_cap = int(candidate_layout.get("queue_spill_capacity", 0))
    spill_storage = int(candidate_layout.get("queue_spill_storage_bytes", 0))
    spill_block = spill_storage // spill_cap if spill_cap else 0
    default_spill_cap = min(args.default_queue, spill_cap)

    memory_default = memory_slot * args.default_queue
    tuning_default = (
        tuning_slot * args.default_queue
        + tuning_spill_block * tuning_default_spill_cap
    )
    candidate_default = (
        candidate_slot * args.default_queue + spill_block * default_spill_cap
    )
    reduction = (
        100.0 * (memory_default - candidate_default) / memory_default
        if memory_default
        else 0.0
    )
    vs_tuning = (
        100.0 * (candidate_default - tuning_default) / tuning_default
        if tuning_default
        else 0.0
    )

    memory = {
        "memory_baseline_ref": args.memory_baseline_ref,
        "throughput_baseline_ref": args.throughput_baseline_ref,
        "default_queue_capacity": args.default_queue,
        "precompact_slot_bytes": memory_slot,
        "tuning_baseline_slot_bytes": tuning_slot,
        "candidate_slot_bytes": candidate_slot,
        "candidate_spill_block_bytes": spill_block,
        "candidate_spill_capacity_at_default": default_spill_cap,
        "precompact_default_queue_bytes": memory_default,
        "tuning_baseline_default_queue_bytes": tuning_default,
        "candidate_default_queue_bytes": candidate_default,
        "memory_reduction_vs_precompact_percent": reduction,
        "memory_change_vs_tuning_percent": vs_tuning,
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
                # 交替执行，降低共享 runner 温度/频率漂移的单向偏差。
                if repeat % 2 == 0:
                    base_samples.append(
                        run_one(
                            args.throughput_baseline_bin,
                            threads,
                            args.records,
                            size,
                        )
                    )
                    cand_samples.append(
                        run_one(args.candidate_bin, threads, args.records, size)
                    )
                else:
                    cand_samples.append(
                        run_one(args.candidate_bin, threads, args.records, size)
                    )
                    base_samples.append(
                        run_one(
                            args.throughput_baseline_bin,
                            threads,
                            args.records,
                            size,
                        )
                    )

            base_prod = median(base_samples, "producer_logs_per_sec")
            cand_prod = median(cand_samples, "producer_logs_per_sec")
            base_e2e = median(base_samples, "end_to_end_logs_per_sec")
            cand_e2e = median(cand_samples, "end_to_end_logs_per_sec")
            base_enqueued = median(base_samples, "enqueued")
            cand_enqueued = median(cand_samples, "enqueued")
            cand_waits = median(cand_samples, "queue_wait_count")
            cand_wakes = median(
                cand_samples, "queue_producer_wake_signals"
            )
            # tuning baseline 在 self-paced wakeup 之前：每次成功 enqueue
            # 都执行一次 cond_signal，因此 old signals == enqueued。
            wake_reduction = (
                1.0 - cand_wakes / base_enqueued
                if base_enqueued
                else 0.0
            )

            overloaded = (
                any_nonzero(base_samples, "dropped")
                or any_nonzero(base_samples, "sync_fallbacks")
                or any_nonzero(base_samples, "queue_spill_exhaustions")
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
                    "producer_ratio": (
                        cand_prod / base_prod
                        if comparable and base_prod
                        else None
                    ),
                    "baseline_end_to_end_logs_per_sec": base_e2e,
                    "candidate_end_to_end_logs_per_sec": cand_e2e,
                    "end_to_end_ratio": (
                        cand_e2e / base_e2e
                        if comparable and base_e2e
                        else None
                    ),
                    "baseline_median_dropped": median(base_samples, "dropped"),
                    "candidate_median_dropped": median(cand_samples, "dropped"),
                    "baseline_median_spill_exhaustions": median(
                        base_samples, "queue_spill_exhaustions"
                    ),
                    "candidate_median_spill_exhaustions": median(
                        cand_samples, "queue_spill_exhaustions"
                    ),
                    "baseline_assumed_wake_signals": base_enqueued,
                    "candidate_median_wait_count": cand_waits,
                    "candidate_median_wake_signals": cand_wakes,
                    "candidate_wake_reduction": wake_reduction,
                    "candidate_wake_per_enqueued": (
                        cand_wakes / cand_enqueued if cand_enqueued else 0.0
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

    md = ["# Queue hotspot benchmark"]
    md.append("")
    md.append(
        f"内存基线: `{args.memory_baseline_ref}`；吞吐调优基线: "
        f"`{args.throughput_baseline_ref}`；每组合 {args.repeats} 次，"
        f"每线程 {args.records} 条。吞吐只用于观察，不作为 CI 性能门禁。"
    )
    md.append("")
    md.append("## 默认 queue 内存门禁")
    md.append("")
    md.append("| 指标 | pre-compact | tuning baseline | candidate |")
    md.append("|---|---:|---:|---:|")
    md.append(
        f"| slot bytes | {memory_slot} | {tuning_slot} | {candidate_slot} |"
    )
    md.append(
        f"| queue capacity={args.default_queue} 预分配 | "
        f"{memory_default} B ({mib(memory_default):.2f} MiB) | "
        f"{tuning_default} B ({mib(tuning_default):.2f} MiB) | "
        f"{candidate_default} B ({mib(candidate_default):.2f} MiB) |"
    )
    md.append(
        f"| candidate spill | - | - | {default_spill_cap} blocks × "
        f"{spill_block} B |"
    )
    md.append("")
    md.append(
        f"candidate 相对 pre-compact 内存下降 **{reduction:.2f}%**；"
        f"门禁 >= **{args.min_memory_reduction:.2f}%**："
        f"{'PASS' if memory['gate_passed'] else 'FAIL'}。"
    )
    md.append(
        f"candidate 相对上一版 compact 固定内存变化：**{vs_tuning:+.2f}%**。"
    )
    md.append("")
    md.append("## 相对上一版 compact 的吞吐")
    md.append("")
    md.append(
        "只有 tuning baseline/candidate 都没有 drop、sync fallback、spill "
        "exhaustion 才计算 ratio；否则标记 overload。"
    )
    md.append("")
    md.append(
        "| threads | bytes | base prod/s | new prod/s | prod ratio | "
        "base e2e/s | new e2e/s | e2e ratio | base spill | new spill | "
        "old wake | new wake | wake↓ | worker waits |"
    )
    md.append("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
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
            f"{row['baseline_median_spill_exhaustions']:.0f} | "
            f"{row['candidate_median_spill_exhaustions']:.0f} | "
            f"{row['baseline_assumed_wake_signals']:.0f} | "
            f"{row['candidate_median_wake_signals']:.0f} | "
            f"{row['candidate_wake_reduction'] * 100.0:.2f}% | "
            f"{row['candidate_median_wait_count']:.0f} |"
        )

    summary = "\n".join(md) + "\n"
    (out / "summary.md").write_text(summary, encoding="utf-8")
    print(summary)

    if not memory["gate_passed"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
