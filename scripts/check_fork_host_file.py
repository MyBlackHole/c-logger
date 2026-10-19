#!/usr/bin/env python3
"""独立验证宿主FILE锁与Logger自有资源非干扰；不声称任意libc安全。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

MODES = ("held", "quiesced")
SUCCESS = re.compile(
    r"^FORK_HOST_FILE_PASS=(held|quiesced) foreign_fd_open=(\d+) "
    r"parent_records=(\d+) child_records=(\d+)$"
)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for part in iter(lambda: f.read(1024 * 1024), b""):
            h.update(part)
    return h.hexdigest()


def verify(mode, code, stdout, stderr):
    if code != 0 or "SKIP=" in stdout or "SKIP=" in stderr:
        raise ValueError("nonzero_or_skipped")
    if any(part in stderr for part in
           ("ERROR: AddressSanitizer", "runtime error:",
            "WARNING: ThreadSanitizer", "LeakSanitizer:")):
        raise ValueError("sanitizer_diagnostic")
    relevant = [line for line in stdout.splitlines()
                if "FORK_HOST_FILE_PASS=" in line]
    if len(relevant) != 1:
        raise ValueError("missing_or_duplicate_success")
    match = SUCCESS.fullmatch(relevant[0])
    if not match or match.groups() != (mode, "1", "2", "2"):
        raise ValueError("invalid_record_or_host_fd_contract")
    return {"mode": mode, "foreign_fd_open": True,
            "parent_records": 2, "child_records": 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    opts = parser.parse_args()
    if opts.repeats < 1 or opts.repeats > 10:
        parser.error("repeats must be 1..10")
    binary = opts.binary.resolve(strict=True)
    opts.output.mkdir(parents=True, exist_ok=False)
    report = {"binary_sha256": sha256(binary), "runs": []}
    try:
        for repeat in range(opts.repeats):
            modes = MODES if repeat % 2 == 0 else tuple(reversed(MODES))
            for mode in modes:
                started = time.monotonic()
                try:
                    proc = subprocess.run(
                        [str(binary), mode], capture_output=True, text=True,
                        check=False, timeout=50)
                except subprocess.TimeoutExpired as exc:
                    name = f"{repeat:02d}-{mode}"
                    stdout = exc.stdout or b""
                    stderr = exc.stderr or b""
                    if isinstance(stdout, str):
                        stdout = stdout.encode("utf-8", errors="replace")
                    if isinstance(stderr, str):
                        stderr = stderr.encode("utf-8", errors="replace")
                    (opts.output / (name + ".stdout")).write_bytes(stdout)
                    (opts.output / (name + ".stderr")).write_bytes(stderr)
                    report["runs"].append({
                        "round": repeat, "mode": mode,
                        "exit_code": None, "timed_out": True,
                        "seconds": time.monotonic() - started,
                    })
                    (opts.output / "results.json").write_text(
                        json.dumps(report, indent=2) + "\n")
                    raise ValueError(f"timeout:{mode}:{repeat}") from exc
                name = f"{repeat:02d}-{mode}"
                (opts.output / (name + ".stdout")).write_text(proc.stdout)
                (opts.output / (name + ".stderr")).write_text(proc.stderr)
                item = {"round": repeat, "mode": mode,
                        "exit_code": proc.returncode,
                        "seconds": time.monotonic() - started}
                report["runs"].append(item)
                (opts.output / "results.json").write_text(
                    json.dumps(report, indent=2) + "\n")
                item["verified"] = verify(
                    mode, proc.returncode, proc.stdout, proc.stderr)
                (opts.output / "results.json").write_text(
                    json.dumps(report, indent=2) + "\n")
                print(f"FORK_HOST_SAMPLE_OK round={repeat} mode={mode}",
                      flush=True)
    except (OSError, ValueError) as exc:
        (opts.output / "failure.txt").write_text(str(exc) + "\n")
        print(f"FORK_HOST_UNVERIFIED={exc}", file=sys.stderr)
        return 1
    count = opts.repeats * len(MODES)
    if len(report["runs"]) != count:
        print("FORK_HOST_UNVERIFIED=incomplete_matrix", file=sys.stderr)
        return 1
    print(f"FORK_HOST_EVIDENCE_OK samples={count}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
