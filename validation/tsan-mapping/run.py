#!/usr/bin/env python3
"""逐项验收原版/候选运行库；预期诊断不是普通程序通过。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

CASES = {
    "no-fork": None,
    "unlocked": None,
    "held": None,
    "retained-order": "lock-order-inversion",
    "failed-map-order": "lock-order-inversion",
    "child-order": "lock-order-inversion",
    "child-destroy-locked": "destroy of a locked mutex",
    "race": "data race",
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--variant", choices=("original", "candidate"), required=True)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    env = os.environ.copy()
    # 不继承宿主的抑制、退出码或运行库覆盖，也不关闭任何检测类别。
    for key in ("TSAN_OPTIONS", "LD_PRELOAD", "LD_LIBRARY_PATH"):
        if env.get(key):
            raise RuntimeError("拒绝外部运行时覆盖: " + key)
    results = []
    for mode, diagnosis in CASES.items():
        if mode == "held" and args.variant == "original":
            diagnosis = "lock-order-inversion"
        repeat = 16 if mode == "held" else 1
        for i in range(repeat):
            cmd = [str(binary), mode]
            timeout = False
            try:
                done = subprocess.run(cmd, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, env=env,
                                      timeout=25, check=False)
                code, output = done.returncode, done.stdout
            except subprocess.TimeoutExpired as e:
                code, output, timeout = None, e.stdout or b"", True
            log = args.output / ("%s-%02d.log" % (mode, i))
            log.write_bytes(output)
            text = output.decode("utf-8", errors="replace")
            expected = 66 if diagnosis else 0
            complete = "PROBE_COMPLETE=" in text or "PROBE_CHILD_COMPLETE=" in text
            matched = code == expected and not timeout and complete
            if diagnosis:
                matched = matched and ("ThreadSanitizer: " + diagnosis) in text
            else:
                matched = matched and "WARNING: ThreadSanitizer" not in text
            if mode.startswith("child-") or mode in ("held", "unlocked"):
                matched = matched and "OLD_ABSENT=1 ADDRESS_REUSED=1" in text
            results.append(dict(case=mode, iteration=i, argv=cmd, exit=code,
                                expected_exit=expected, expected_diagnostic=diagnosis,
                                log=log.name, timeout=timeout, accepted=bool(matched)))
    summary = dict(variant=args.variant,
                   binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                   results=results, accepted=all(r["accepted"] for r in results))
    (args.output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"variant": args.variant, "accepted": summary["accepted"],
                      "runs": len(results)}))
    return 0 if summary["accepted"] else 1


if __name__ == "__main__":
    sys.exit(main())
