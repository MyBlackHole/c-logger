#!/usr/bin/env python3
"""独立生产库 fork+异步worker 压力执行器：拒绝跳过/非零/缺失输出。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

MODES = ("serial", "parallel")
MARKER = re.compile(
    r"^FORK_WORKER_STRESS_PASS=(serial|parallel) "
    r"children=(\d+) parent_records=(\d+) child_records=(\d+) "
    r"parent_progress_checks=(\d+)$"
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_result(mode, exit_code, stdout, stderr):
    if exit_code != 0 or "SKIP=" in stdout or "SKIP=" in stderr:
        raise ValueError("nonzero_or_skipped")
    if any(word in stderr for word in
           ("ERROR: AddressSanitizer", "runtime error:",
            "WARNING: ThreadSanitizer", "LeakSanitizer:")):
        raise ValueError("sanitizer_reported_failure")
    matches = [MARKER.fullmatch(line) for line in stdout.splitlines()
               if "FORK_WORKER_STRESS_PASS=" in line]
    if len(matches) != 1 or matches[0] is None:
        raise ValueError("missing_or_duplicate_complete_marker")
    found_mode, children, parent, child, progress = matches[0].groups()
    if (found_mode != mode or int(children) != 4 or int(parent) < 4
            or int(child) != 136 or int(progress) != 16):
        raise ValueError("unexpected_fork_worker_counts")
    return {"mode": found_mode, "children": int(children),
            "parent_records": int(parent), "child_records": int(child),
            "parent_progress_checks": int(progress)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10:
        parser.error("repeats_out_of_range")
    binary = args.binary.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    evidence = {"binary": str(binary), "binary_sha256": sha256(binary),
                "repeats": args.repeats, "results": []}
    try:
        for round_number in range(args.repeats):
            # 奇数轮反转顺序，降低固定先后顺序造成的夹具偏差。
            order = MODES if round_number % 2 == 0 else tuple(reversed(MODES))
            for mode in order:
                started = time.monotonic()
                try:
                    result = subprocess.run(
                        [str(binary), mode], capture_output=True, text=True,
                        check=False, timeout=90)
                except subprocess.TimeoutExpired as err:
                    raise ValueError(f"timeout:{mode}:{round_number}") from err
                name = f"{round_number:02d}-{mode}"
                (args.out / (name + ".stdout")).write_text(result.stdout)
                (args.out / (name + ".stderr")).write_text(result.stderr)
                item = {"round": round_number, "mode": mode,
                        "exit_code": result.returncode,
                        "elapsed_seconds": time.monotonic() - started}
                evidence["results"].append(item)
                (args.out / "results.json").write_text(
                    json.dumps(evidence, indent=2) + "\n")
                item["verified"] = parse_result(
                    mode, result.returncode, result.stdout, result.stderr)
                (args.out / "results.json").write_text(
                    json.dumps(evidence, indent=2) + "\n")
                print(f"FORK_STRESS_SAMPLE_OK round={round_number} mode={mode}",
                      flush=True)
        if len(evidence["results"]) != args.repeats * len(MODES):
            raise ValueError("incomplete_sample_matrix")
    except (OSError, ValueError) as err:
        (args.out / "failure.txt").write_text(str(err) + "\n")
        print(f"FORK_STRESS_UNVERIFIED={err}", file=sys.stderr)
        return 1
    print(f"FORK_STRESS_EVIDENCE_OK samples={len(evidence['results'])}",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
