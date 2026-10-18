#!/usr/bin/env python3
"""固定到达率公开 API 基准：受限运行、原始样本、计数与文件内容核验。"""
import array
import datetime
import errno
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from tool_i18n import parser_for

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 1024 ** 3
RAW_MAGIC = b"CLFR\x02\x00\x00\x00"
RECORD = re.compile(rb"FR phase=([01]) seq=(\d+) payload=(x+)\n$")


def command(*args):
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
    return {"command": list(args), "returncode": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compress(source, target):
    partial = target.with_name(target.name + ".partial")
    with open(source, "rb") as src, open(partial, "wb") as archive:
        with gzip.GzipFile(filename=target.name, mode="wb", fileobj=archive, compresslevel=1) as dst:
            shutil.copyfileobj(src, dst, 1024 * 1024)
    # 包括 gzip 尾部关闭在内均成功，才以完整归档的名字发布。
    os.replace(partial, target)


def check_evidence_budget(output):
    used = sum(p.stat().st_size for p in output.rglob("*") if p.is_file())
    if used <= MAX_BYTES:
        return None
    failure = {"verification_valid": False, "error": "evidence_budget_exceeded",
               "observed_bytes": used, "limit_bytes": MAX_BYTES}
    # 先输出回执；磁盘已满时，控制台仍有失败原因。
    print(json.dumps(failure), file=sys.stderr, flush=True)
    (output / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
    return failure


def read_raw(path):
    phases = {}
    with open(path, "rb") as stream:
        if stream.read(len(RAW_MAGIC)) != RAW_MAGIC:
            raise ValueError("raw_schema_mismatch")
        while header := stream.read(16):
            if len(header) != 16:
                raise ValueError("truncated_raw_header")
            values = array.array("Q")
            values.frombytes(header)
            number, count = values
            if number not in (0, 1) or number in phases or not 1 <= count <= 2000000:
                raise ValueError("invalid_raw_header")
            if count + sum(len(value[0]) for value in phases.values()) > 2000000:
                raise ValueError("raw_slot_budget_exceeded")
            calls, late, status = array.array("Q"), array.array("Q"), array.array("q")
            try:
                calls.fromfile(stream, count)
                late.fromfile(stream, count)
                status.fromfile(stream, count)
            except EOFError as exc:
                raise ValueError("truncated_raw_arrays") from exc
            phases[number] = (calls, late, status)
    if not phases:
        raise ValueError("missing_raw_phases")
    return phases


def verify_raw(phases, rows):
    """原始槽位数据独立重算分位；未发出槽位不得作为零延迟。"""
    summaries = {}
    if len(rows) != len(phases) or set(phases) != {row["phase"] for row in rows}:
        raise ValueError("raw_phase_mismatch")
    for row in rows:
        calls, late, status = phases[row["phase"]]
        if row["raw_schema"] != 2 or len(calls) != len(late) or len(calls) != len(status):
            raise ValueError("raw_schema_or_length_mismatch")
        if any(not calls[i] and (late[i] or status[i]) for i in range(len(calls))):
            raise ValueError("raw_unattempted_slot_has_data")
        if (row["scheduled"] != row["attempted"] + row["generator_missed"] or
                row["attempted"] != row["enqueued"] + row["sync_fallbacks"] + row["dropped"] or
                row["emitted"] + row["failed"] != row["enqueued"] + row["sync_fallbacks"]):
            raise ValueError("raw_accounting_mismatch")
        indices = [i for i, value in enumerate(calls) if value]
        if len(calls) != row["scheduled"] or len(indices) != row["attempted"]:
            raise ValueError("raw_count_mismatch")
        success = [i for i in indices if status[i] == 0]
        eagain = sum(status[i] == -errno.EAGAIN for i in indices)
        other = len(indices) - len(success) - eagain
        if (row["api_success"] != len(success) or row["api_eagain"] != eagain or
                row["api_other_errors"] != other or other or
                len(success) != row["enqueued"] + row["sync_fallbacks"] or eagain != row["dropped"]):
            raise ValueError("raw_status_accounting_mismatch")
        for key, values, selected in (("api_ns", calls, indices),
                                      ("arrival_late_ns", late, indices),
                                      ("api_success_ns", calls, success)):
            ordered = sorted(values[i] for i in selected)
            n = len(ordered)
            expected = {f"p{pct}": ordered[(n * pct + 99) // 100 - 1] if n else 0
                        for pct in (50, 95, 99)}
            expected["max"] = ordered[-1] if n else 0
            if key == "api_success_ns":
                if not n:
                    expected = dict.fromkeys(expected)
                expected["count"] = n
            if expected != row[key]:
                raise ValueError("raw_percentile_mismatch")
        if sum(late[i] > 1000000 for i in indices) != row["late_over_1ms"]:
            raise ValueError("raw_lateness_mismatch")
        summaries[row["phase"]] = {"scheduled": len(calls), "attempted": len(indices),
                                    "api_success": len(success), "api_eagain": eagain,
                                    "api_other_errors": other}
    return summaries


def verify_file(path, phases, rows, payload_bytes):
    seen = {phase: bytearray(len(values[0])) for phase, values in phases.items()}
    counts = {phase: 0 for phase in phases}
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for line in stream:
            digest.update(line)
            match = RECORD.search(line)
            if not match or len(match[3]) != payload_bytes:
                raise ValueError("file_record_corrupt")
            phase, seq = int(match[1]), int(match[2])
            if phase not in seen or seq >= len(seen[phase]) or seen[phase][seq]:
                raise ValueError("file_record_duplicate_or_out_of_range")
            if not phases[phase][0][seq]:
                raise ValueError("file_record_not_attempted")
            if phases[phase][2][seq] != 0:
                raise ValueError("file_record_not_successful")
            seen[phase][seq] = 1
            counts[phase] += 1
    for row in rows:
        if counts[row["phase"]] != row["emitted"]:
            raise ValueError("file_emitted_count_mismatch")
    return {"verified": True, "records": counts, "bytes": path.stat().st_size,
            "sha256": digest.hexdigest()}


def wait_exit_code(status):
    # Ubuntu 20.04 的 Python 3.8 尚无 os.waitstatus_to_exitcode。
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    if os.WIFSIGNALED(status):
        return -os.WTERMSIG(status)
    raise ValueError("unexpected_wait_status")


def child_limits():
    # 文件与地址空间双重封顶；外层还限制完整进程生命周期。
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_BYTES, MAX_BYTES))
    resource.setrlimit(resource.RLIMIT_AS, (2 * MAX_BYTES, 2 * MAX_BYTES))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run_sample(binary, output, case, timeout, temp_root):
    output.mkdir()
    start = datetime.datetime.now(datetime.timezone.utc).isoformat()
    temp = Path(tempfile.mkdtemp(prefix="logger-fixed-rate-", dir=temp_root)).resolve()
    cleanup_temp = False
    result = {}
    try:
        path = temp / "output.log" if case["backend"] == "file" else Path("/dev/null")
        raw = temp / "latency.bin"
        args = [str(binary), str(case["threads"]), str(case["rate"]),
                str(case["seconds"]), str(case["warmup"]),
                str(case["payload_bytes"]), str(path), str(raw)]
        required = case["rate"] * (case["seconds"] + case["warmup"]) * (case["payload_bytes"] + 536)
        # 临时原件与最坏压缩输出可能同时存在；同盘时保留两份预算。
        for directory in (temp, output):
            if shutil.disk_usage(directory).free < 2 * required + 256 * 1024 ** 2:
                raise RuntimeError("insufficient_free_space")
        wall_start = time.monotonic()
        timed_out = False
        with (output / "stdout.jsonl").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(args, stdout=stdout, stderr=stderr,
                                       start_new_session=True, preexec_fn=child_limits)
            runner_error = None
            try:
                while True:
                    pid, status, usage = os.wait4(process.pid, os.WNOHANG)
                    if pid:
                        break
                    if time.monotonic() - wall_start > timeout:
                        timed_out = True
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        _, status, usage = os.wait4(process.pid, 0)
                        break
                    time.sleep(0.02)
            except BaseException as exc:
                # 包括 Ctrl-C：先结束自己启动的进程组并回收，再清理临时目录。
                runner_error = type(exc).__name__
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                _, status, usage = os.wait4(process.pid, 0)
            process.returncode = wait_exit_code(status)
        result = {"case": case, "command": args, "started_utc": start,
                  "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "lifecycle_wall_seconds": time.monotonic() - wall_start,
                  "lifecycle_user_seconds": usage.ru_utime,
                  "lifecycle_system_seconds": usage.ru_stime,
                  "lifecycle_maxrss_kib": usage.ru_maxrss,
                  "returncode": process.returncode, "timed_out": timed_out,
                  "runner_error": runner_error,
                  "filesystem": command("stat", "-f", "-c", "%T", str(temp)),
                  "verification_valid": False}
        try:
            parsed = [json.loads(line) for line in (output / "stdout.jsonl").read_text().splitlines()]
            if not parsed or any(not isinstance(row, dict) for row in parsed):
                raise ValueError("invalid_result_rows")
            rows = [row for row in parsed if "phase" in row]
            expected_phases = [0, 1] if case["warmup"] else [1]
            if [row["phase"] for row in rows] != expected_phases:
                raise ValueError("missing_phase_results")
            result["phases"] = rows
            if (process.returncode or timed_out or runner_error or
                    (output / "stderr.log").stat().st_size or
                    not all(row["valid"] is True for row in rows)):
                raise ValueError("child_run_invalid")
            expected_close = 0 if case["backend"] == "file" else -errno.EINVAL
            if (len(parsed) != len(expected_phases) + 2 or
                    parsed[-1].get("close_rc") not in (0, expected_close) or
                    parsed[-1].get("close_errno") != -parsed[-1]["close_rc"]):
                raise ValueError("child_run_invalid")
            phases = read_raw(raw)
            result["raw_verification"] = verify_raw(phases, rows)
            result["phases"] = rows
            result["close"] = parsed[-1]
            result["clock_pair_ns"] = parsed[0]["clock_pair_ns"]
            if case["backend"] == "file":
                result["file_verification"] = verify_file(path, phases, rows, case["payload_bytes"])
            result["verification_valid"] = True
        except (ValueError, OSError, EOFError, KeyError, TypeError, IndexError) as exc:
            result["verification_error"] = str(exc)
        # 归档失败时保留原件；不能因退出临时目录的上下文而删除唯一完整副本。
        for source, name in ((raw, "latency.bin.gz"), (path, "output.log.gz")):
            if source != Path("/dev/null") and source.exists():
                try:
                    compress(source, output / name)
                except OSError as exc:
                    result["verification_valid"] = False
                    result["archive_error"] = {"artifact": name, "errno": exc.errno,
                                               "message": str(exc)}
                    result["retained_temp_dir"] = str(temp)
                    break
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        cleanup_temp = "archive_error" not in result
    finally:
        if cleanup_temp or not any(temp.iterdir()):
            shutil.rmtree(temp)
        else:
            # 回执本身写失败或归档中断时，也必须能定位尚未删除的原件。
            receipt = dict(result, error="sample_evidence_retained",
                           verification_valid=False, retained_temp_dir=str(temp))
            print(json.dumps(receipt), file=sys.stderr, flush=True)
    return result


def terminate_requested(signum, frame):
    raise KeyboardInterrupt("termination_requested")


def main():
    signal.signal(signal.SIGTERM, terminate_requested)
    parser, _ = parser_for("Run bounded fixed-rate public API benchmarks")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--build-log", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--rates", type=int, nargs="+", default=[10000, 50000, 100000])
    parser.add_argument("--payload-bytes", type=int, nargs="+", default=[64, 1024])
    parser.add_argument("--backends", choices=["null", "file"], nargs="+", default=["null", "file"])
    parser.add_argument("--seconds", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--temp-root", type=Path)
    args = parser.parse_args()
    if (not 1 <= args.seconds <= 30 or not 0 <= args.warmup <= 5 or
            not 1 <= args.repeats <= 10 or not 10 <= args.timeout <= 120 or
            any(not 1 <= t <= 64 for t in args.threads) or
            any(not 1 <= r <= 2000000 for r in args.rates) or
            any(not 1 <= s <= 3500 for s in args.payload_bytes)):
        parser.error("invalid_parameters")
    cases = []
    for repeat in range(args.repeats):
        batch = [{"threads": t, "rate": r, "payload_bytes": s, "backend": backend,
                  "seconds": args.seconds, "warmup": args.warmup, "repeat": repeat}
                 for t in args.threads for s in args.payload_bytes
                 for backend in args.backends for r in args.rates]
        cases.extend(reversed(batch) if repeat % 2 else batch)
    if len(cases) > 200:
        parser.error("case_budget_exceeded")
    for case in cases:
        count = case["rate"] * (case["seconds"] + case["warmup"])
        if count > 2000000 or count * (case["payload_bytes"] + 512) > MAX_BYTES:
            parser.error("sample_budget_exceeded")
    binary = args.binary.resolve(strict=True)
    library = args.library.resolve(strict=True)
    build_log = args.build_log.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    metadata = {"argv": sys.argv, "binary": str(binary), "binary_sha256": sha256(binary),
                "library": str(library), "library_sha256": sha256(library),
                "xmake_config": {str(p.relative_to(ROOT)): p.read_text() for p in (ROOT / ".xmake").rglob("xmake.conf")},
                "source_files_sha256": {str(p.relative_to(ROOT)): sha256(p)
                    for pattern in ("src/*.c", "src/*.h", "include/*.h", "tests/bench_fixed_rate.c",
                                    "scripts/benchmark_fixed_rate.py", "xmake.lua")
                    for p in ROOT.glob(pattern)},
                "raw_schema": 2,
                "raw_format": "CLFR 02 00 00 00; native uint64 phase,count,calls[count],late[count]; int64 status[count]; repeated phases",
                "byteorder": sys.byteorder, "platform": platform.platform(),
                "cpu_affinity": sorted(os.sched_getaffinity(0)),
                "source": command("git", "rev-parse", "HEAD"),
                "tree": command("git", "rev-parse", "HEAD^{tree}"),
                "status": command("git", "status", "--porcelain"),
                "diff": command("git", "diff", "HEAD"),
                "cpu": Path("/proc/cpuinfo").read_text(),
                "mounts": Path("/proc/mounts").read_text(),
                "compiler": command("cc", "--version"),
                "resolved_dependencies": command("ldd", str(binary)),
                "elf_dynamic": command("readelf", "-d", str(binary))}
    # 共享模式必须核对实际加载路径，而不是只记录使用者给出的库文件。
    for line in metadata["resolved_dependencies"]["stdout"].splitlines():
        if "liblogger.so" in line and "=>" in line:
            resolved = line.split("=>", 1)[1].strip().split(" (", 1)[0]
            if Path(resolved).resolve() != library:
                raise ValueError("resolved_library_mismatch")
    shutil.copyfile(build_log, args.out / "build.log")
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if check_evidence_budget(args.out):
        return 1
    results = []
    for index, case in enumerate(cases):
        output = args.out / f"sample-{index:03}"
        result = run_sample(binary, output, case, args.timeout, args.temp_root)
        results.append(result)
        with (args.out / "results.jsonl").open("a") as stream:
            stream.write(json.dumps(result) + "\n")
        # 包含本轮归档及两份回执；最后一个样本也必须接受总预算判定。
        if check_evidence_budget(args.out):
            result["verification_valid"] = False
            result["evidence_error"] = "evidence_budget_exceeded"
            (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            (args.out / "results.jsonl").write_text(
                "".join(json.dumps(row) + "\n" for row in results))
        print(json.dumps({"sample": index, "case": case,
                          "verification_valid": result["verification_valid"]}), flush=True)
        if not result["verification_valid"]:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
