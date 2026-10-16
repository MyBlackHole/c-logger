#!/usr/bin/env python3
"""固定 ASan 运行库的可执行验收；不链接生产库、不读取私有锁。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from tool_i18n import parser_for


def detection_ok(kind: str, code: int, output: str) -> bool:
    if kind == "healthy":
        return code == 0 and output.strip() == "SANITIZER_HEALTHY=1"
    marker = {"heap-overflow": "ERROR: AddressSanitizer: heap-buffer-overflow",
              "signed-overflow": "runtime error: signed integer overflow"}[kind]
    return (code == 1 and marker in output
            and "SANITIZER_MISSED_ERROR" not in output)


def fork_ok(code: int, output: str) -> bool:
    registration = re.findall(r"^SANITIZER_ATFORK registrations=(\d+) dropped=(\d+)$",
                              output, re.M)
    results = re.findall(r"^FORK_PROBE_ITERATION=(\d+) status=(\d+)$", output, re.M)
    summary = re.findall(r"^FORK_PROBE completed=(\d+) timeout=(\d+) failed=(\d+)$",
                         output, re.M)
    return (code == 0 and registration == [("1", "0")]
            and results == [(str(i), "0") for i in range(96)]
            and summary == [("96", "0", "0")])


def run(command: list[str], path: Path, timeout: int) -> dict:
    begin = time.monotonic()
    with path.open("wb") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        expired = False
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            expired = True
            os.killpg(proc.pid, signal.SIGKILL)
            code = proc.wait()
        finally:
            # 只清理由本次自检创建的独立进程组，不按程序名清理其他任务。
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    return {"command": command, "exit": code, "timeout": expired,
            "elapsed": time.monotonic() - begin, "log": path.name}


def main() -> int:
    parser, _ = parser_for("Qualify the ASan fork runtime without suppressing errors")
    parser.add_argument("--fork-binary", required=True, type=Path)
    parser.add_argument("--detection-binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "passed": False, "runs": [], "binaries": {}}
    try:
        # 这不是禁用检测器选项的入口：正式自检要求其默认策略。
        for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
            if os.environ.get(key):
                raise ValueError("SANITIZER_OPTIONS_NOT_DEFAULT:" + key)
        for name in ("fork_binary", "detection_binary"):
            binary = getattr(args, name).resolve(strict=True)
            setattr(args, name, binary)
            report["binaries"][name] = {"path": str(binary),
                                        "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        # 固定 Linux/Clang 静态 ASan 自检产物必须保留该上游实现符号。
        # 只检查已链接产物，不调用私有接口；符号存在不是动态通过的替代。
        symbols = run(["nm", "--defined-only", "--demangle", str(args.fork_binary)],
                      args.output / "symbols.log", 30)
        report["runs"].append(symbols)
        if symbols["exit"] or "__asan::InstallAtForkHandler()" not in (
                args.output / "symbols.log").read_text(errors="replace"):
            raise ValueError("ASAN_FORK_IMPLEMENTATION_MISSING")
        good = True
        for kind in ("healthy", "heap-overflow", "signed-overflow"):
            item = run([str(args.detection_binary), kind], args.output / (kind + ".log"), 30)
            item["accepted"] = not item["timeout"] and detection_ok(
                kind, item["exit"], (args.output / item["log"]).read_text(errors="replace"))
            good = good and item["accepted"]
            report["runs"].append(item)
        item = run([str(args.fork_binary)], args.output / "fork-stress.log", 220)
        item["accepted"] = not item["timeout"] and fork_ok(
            item["exit"], (args.output / item["log"]).read_text(errors="replace"))
        report["runs"].append(item)
        report["passed"] = good and item["accepted"]
    except (OSError, ValueError) as error:
        report["error"] = str(error)
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print("ASAN_RUNTIME_QUALIFIED=" + str(int(report["passed"])))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
