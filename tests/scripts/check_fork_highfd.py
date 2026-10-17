#!/usr/bin/env python3
"""高 fd 的独立证据：退出 77 / 跳过标记必须失败，不使用 Xmake 跳过映射。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import sys
import time

MIN_REQUIRED = 32769
MODES = ("grow", "grow-failure")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for part in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(part)
    return digest.hexdigest()


def verified(result, mode):
    if result.returncode != 0 or "FORK_FD_SKIP=" in result.stdout:
        return False
    return re.search(r"(?m)^FORK_FD_PASS=" + re.escape(mode) + r"$",
                     result.stdout) is not None


def selftest():
    from subprocess import CompletedProcess
    assert verified(CompletedProcess([], 0, "FORK_FD_PASS=grow\n", ""), "grow")
    for code, text in ((77, "FORK_FD_SKIP=high-fd hard=16384\n"),
                       (0, "FORK_FD_SKIP=high-fd hard=16384\n"),
                       (0, "FORK_FD_PASS=grow-failure\n"),
                       (1, "FORK_FD_PASS=grow\n"),
                       (0, "")):
        assert not verified(CompletedProcess([], code, text, ""), "grow")


def main():
    parser = argparse.ArgumentParser(description="验证真实高fd扩容及错误回滚")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--binary", type=Path, action="append", default=[])
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.selftest:
        selftest()
        print("HIGHFD_ORACLE_SELFTEST_OK")
        return 0
    if len(args.binary) != 2 or args.out is None:
        parser.error("require two --binary and --out")
    args.out.mkdir(parents=True, exist_ok=False)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    meta = {"rlimit_soft_before": soft, "rlimit_hard": hard,
            "required_exclusive_upper_bound": MIN_REQUIRED,
            "platform": sys.platform, "machine": os.uname().machine}
    try:
        if hard != resource.RLIM_INFINITY and hard < MIN_REQUIRED:
            raise RuntimeError("high_fd_hard_limit_insufficient")
        if soft < MIN_REQUIRED:
            resource.setrlimit(resource.RLIMIT_NOFILE, (MIN_REQUIRED, hard))
        meta["rlimit_soft_effective"] = resource.getrlimit(resource.RLIMIT_NOFILE)[0]
        (args.out / "environment.json").write_text(
            json.dumps(meta, indent=2) + "\n", encoding="utf-8")
        results = []
        for binary in args.binary:
            path = binary.resolve(strict=True)
            name = path.name
            if name not in ("fork_fd_production", "fork_fd_regression"):
                raise RuntimeError("unexpected_binary_" + name)
            for mode in MODES:
                start = time.monotonic()
                try:
                    completed = subprocess.run(
                        [str(path), mode], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, timeout=30,
                        check=False)
                except subprocess.TimeoutExpired as exc:
                    raise RuntimeError(f"{name}/{mode}: timeout") from exc
                basename = f"{name}-{mode}"
                (args.out / (basename + ".stdout")).write_text(
                    completed.stdout, encoding="utf-8")
                (args.out / (basename + ".stderr")).write_text(
                    completed.stderr, encoding="utf-8")
                item = {"binary": name, "binary_sha256": sha256(path),
                        "scenario": mode, "exit_code": completed.returncode,
                        "duration_seconds": time.monotonic() - start,
                        "verified": verified(completed, mode)}
                results.append(item)
                (args.out / "results.json").write_text(
                    json.dumps(results, indent=2) + "\n", encoding="utf-8")
                if not item["verified"]:
                    raise RuntimeError(f"{basename}: not proven (exit={completed.returncode})")
        if len(results) != 4 or not all(x["verified"] for x in results):
            raise RuntimeError("incomplete_high_fd_matrix")
        print("HIGHFD_PROVEN=4/4", flush=True)
        return 0
    except (OSError, RuntimeError) as exc:
        (args.out / "failure.txt").write_text(str(exc) + "\n", encoding="utf-8")
        print(f"HIGHFD_UNVERIFIED={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
