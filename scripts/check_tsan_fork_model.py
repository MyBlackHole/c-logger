#!/usr/bin/env python3
"""TSan 旧锁映射失效模型：真实绑定正例及新域错误反例均不得省略。"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # 只设确定的错误退出码、立即停止及诊断详细度；不关闭任何检测类别。
    env["TSAN_OPTIONS"] = "halt_on_error=1:exitcode=66:second_deadlock_stack=1:verbosity=1"
    cases = [("healthy", 0, None),
             ("order-error", 66, "WARNING: ThreadSanitizer: lock-order-inversion"),
             ("race-error", 66, "WARNING: ThreadSanitizer: data race")]
    results = []
    for name, expected, warning in cases:
        try:
            run = subprocess.run([str(binary), name], env=env, capture_output=True,
                                 text=True, timeout=30, check=False)
            text = run.stdout + run.stderr
            good = run.returncode == expected and "parent had 1 threads" in text
            good &= warning in text if warning else "WARNING: ThreadSanitizer" not in text
            record = {"case": name, "exit_code": run.returncode,
                      "expected_exit": expected, "accepted": bool(good)}
        except subprocess.TimeoutExpired as exc:
            text = repr(exc)
            record = {"case": name, "timeout": True, "accepted": False}
        (args.output / (name + ".log")).write_text(text)
        results.append(record)
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, separators=(",", ":")))
    return 0 if all(r["accepted"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
