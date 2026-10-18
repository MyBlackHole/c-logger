#!/usr/bin/env python3
"""固定速率验收器必须拒绝伪造计数、分位和缺失/重复/损坏输出。"""
import array
import copy
import errno
import json
import os
import shlex
import subprocess
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("fixed_rate", ROOT / "scripts/benchmark_fixed_rate.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class Verification(unittest.TestCase):
    def setUp(self):
        self.phases = {1: (array.array("Q", [10, 20, 0]), array.array("Q", [0, 2000000, 0]),
                           array.array("q", [0, -errno.EAGAIN, 0]))}
        self.row = {"raw_schema": 2, "api_success": 1, "api_eagain": 1, "api_other_errors": 0,
                    "api_success_ns": {"count": 1, "p50": 10, "p95": 10, "p99": 10, "max": 10},
                    "phase": 1, "scheduled": 3, "attempted": 2, "generator_missed": 1,
                    "enqueued": 1, "sync_fallbacks": 0, "dropped": 1, "emitted": 1,
                    "failed": 0, "late_over_1ms": 1,
                    "api_ns": {"p50": 10, "p95": 20, "p99": 20, "max": 20},
                    "arrival_late_ns": {"p50": 0, "p95": 2000000, "p99": 2000000, "max": 2000000}}

    def test_wait_status_compatibility(self):
        for code in (0, 1, 42, 255):
            self.assertEqual(bench.wait_exit_code(code << 8), code)
        for signum in (9, 15):
            self.assertEqual(bench.wait_exit_code(signum), -signum)
        with self.assertRaises(ValueError):
            bench.wait_exit_code(0x7f)

    def test_raw_valid(self):
        bench.verify_raw(self.phases, [self.row])

    def test_raw_tamper(self):
        for key in ("scheduled", "attempted", "generator_missed", "enqueued", "dropped", "emitted", "late_over_1ms"):
            row = copy.deepcopy(self.row)
            row[key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                bench.verify_raw(self.phases, [row])
        row = copy.deepcopy(self.row)
        row["api_ns"]["p99"] = 10
        with self.assertRaises(ValueError):
            bench.verify_raw(self.phases, [row])

    def check_file(self, contents):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "output.log"
            path.write_bytes(contents)
            return bench.verify_file(path, self.phases, [self.row], 3)

    def test_file_valid(self):
        self.assertTrue(self.check_file(b"prefix FR phase=1 seq=0 payload=xxx\n")["verified"])

    def test_file_rejects(self):
        line = b"prefix FR phase=1 seq=0 payload=xxx\n"
        for data in (b"", line + line, line[:-1], line.replace(b"xxx", b"xxz"),
                     line.replace(b"seq=0", b"seq=2"), line.replace(b"seq=0", b"seq=3"),
                     line.replace(b"phase=1", b"phase=0")):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.check_file(data)

    def test_raw_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "raw"
            with path.open("wb") as stream:
                stream.write(bench.RAW_MAGIC)
                array.array("Q", [1, 3]).tofile(stream)
                for values in self.phases[1]:
                    values.tofile(stream)
            self.assertEqual(bench.read_raw(path), self.phases)
            path.write_bytes(b"bad")
            with self.assertRaises(ValueError):
                bench.read_raw(path)

    def test_status_counts_and_success_percentiles(self):
        for key in ("api_success", "api_eagain", "api_other_errors"):
            row = copy.deepcopy(self.row)
            row[key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "raw_status"):
                bench.verify_raw(self.phases, [row])
        for key in ("count", "p50", "p95", "p99", "max"):
            row = copy.deepcopy(self.row)
            row["api_success_ns"][key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                bench.verify_raw(self.phases, [row])

    def test_other_status_never_becomes_valid_sample(self):
        for code in (-errno.EIO, -errno.ENODEV, 1):
            phases = copy.deepcopy(self.phases)
            phases[1][2][1] = code
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, "raw_status"):
                bench.verify_raw(phases, [self.row])

    def test_unattempted_status_or_lateness_rejected(self):
        for index in (1, 2):
            phases = copy.deepcopy(self.phases)
            phases[1][index][2] = 1
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "unattempted"):
                bench.verify_raw(phases, [self.row])

    def test_zero_success_has_no_latency(self):
        phases = copy.deepcopy(self.phases)
        phases[1][2][0] = -errno.EAGAIN
        row = copy.deepcopy(self.row)
        row.update(api_success=0, api_eagain=2, enqueued=0, dropped=2, emitted=0)
        row["api_success_ns"] = {"count": 0, "p50": None, "p95": None, "p99": None, "max": None}
        bench.verify_raw(phases, [row])
        row["api_success_ns"]["p99"] = 0
        with self.assertRaises(ValueError):
            bench.verify_raw(phases, [row])

    def test_output_cannot_substitute_dropped_slot(self):
        with self.assertRaisesRegex(ValueError, "file_record_not_successful"):
            self.check_file(b"prefix FR phase=1 seq=1 payload=xxx\n")

    def test_raw_schema_and_truncation(self):
        header = array.array("Q", [1, 3]).tobytes()
        body = b"".join(a.tobytes() for a in self.phases[1])
        valid = bench.RAW_MAGIC + header + body
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "raw"
            cases = (header + body, bench.RAW_MAGIC, valid[:-1], valid[:35],
                     valid + b"x", valid + header + body,
                     bench.RAW_MAGIC + array.array("Q", [1, 0]).tobytes())
            for data in cases:
                path.write_bytes(data)
                with self.subTest(length=len(data)), self.assertRaises(ValueError):
                    bench.read_raw(path)
        with self.assertRaisesRegex(ValueError, "raw_phase_mismatch"):
            bench.verify_raw(self.phases, [self.row, self.row])

    def test_invalid_child_receipts_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for variant in ("healthy", "diagnostic", "close-error", "api-error", "bad-json"):
                row = dict(copy.deepcopy(self.row), valid=True)
                arrays = copy.deepcopy(self.phases[1])
                close = {"close_rc": 0, "close_errno": 0}
                diagnostic = "runtime error: synthetic\n" if variant == "diagnostic" else ""
                if variant == "close-error":
                    close = {"close_rc": -errno.EIO, "close_errno": errno.EIO}
                if variant == "api-error":
                    arrays[2][1] = -errno.EIO
                    row.update(api_eagain=0, api_other_errors=1)
                rows = [{"clock_pair_ns": {"p50": 1, "p99": 2}}, row, close]
                if variant == "bad-json":
                    rows[0] = []
                raw = bench.RAW_MAGIC + array.array("Q", [1, 3]).tobytes()
                raw += b"".join(a.tobytes() for a in arrays)
                output = "".join(json.dumps(value) + "\n" for value in rows)
                script = ("#!/usr/bin/env python3\nimport pathlib,sys\n"
                          f"pathlib.Path(sys.argv[7]).write_bytes(bytes.fromhex({raw.hex()!r}))\n"
                          "pathlib.Path(sys.argv[6]).write_bytes(b'prefix FR phase=1 seq=0 payload=xxx\\n')\n"
                          f"sys.stdout.write({output!r})\nsys.stderr.write({diagnostic!r})\n")
                binary = root / "fixture"
                binary.write_text(script)
                binary.chmod(0o755)
                case = {"threads": 1, "rate": 3, "seconds": 1, "warmup": 0,
                        "payload_bytes": 3, "backend": "file", "repeat": 0}
                result = bench.run_sample(binary, root / variant, case, 2, None)
                with self.subTest(variant=variant):
                    self.assertEqual(result["verification_valid"], variant == "healthy", result)
                    self.assertTrue((root / variant / "latency.bin.gz").exists())
                    self.assertTrue((root / variant / "output.log.gz").exists())

    def test_timeout_cleans_own_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "blocked"
            binary.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
            binary.chmod(0o755)
            owned = root / "temporary"
            owned.mkdir()
            case = {"threads": 1, "rate": 1, "seconds": 1, "warmup": 0,
                    "payload_bytes": 3, "backend": "file", "repeat": 0}
            result = bench.run_sample(binary, root / "result", case, 0.1, owned)
            self.assertTrue(result["timed_out"])
            self.assertFalse(result["verification_valid"])
            self.assertEqual(list(owned.iterdir()), [])
            self.assertTrue((root / "result/stdout.jsonl").exists())

    def test_runner_exception_reaps_child(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "blocked"
            binary.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
            binary.chmod(0o755)
            owned = root / "temporary"
            owned.mkdir()
            case = {"threads": 1, "rate": 1, "seconds": 1, "warmup": 0,
                    "payload_bytes": 3, "backend": "file", "repeat": 0}
            original = bench.os.wait4
            calls = []
            def interrupted(pid, flags):
                calls.append(pid)
                if len(calls) == 1:
                    raise KeyboardInterrupt()
                return original(pid, flags)
            with mock.patch.object(bench.os, "wait4", side_effect=interrupted):
                result = bench.run_sample(binary, root / "result", case, 1, owned)
            self.assertEqual(result["runner_error"], "KeyboardInterrupt")
            self.assertFalse(result["verification_valid"])
            self.assertEqual(list(owned.iterdir()), [])
            with self.assertRaises(ProcessLookupError):
                bench.os.kill(calls[0], 0)


PRODUCER_FIXTURE = r'''#define main unused_benchmark_main
#define logger_log injected_logger_log
#include "driver.c"
#undef logger_log
#undef main
static const int results[] = { 0, -EAGAIN, -EIO };
static unsigned used;
int injected_logger_log(logger_t *l, logger_level_t level, const char *module,
                        const char *file, int line, const char *func,
                        const char *fmt, ...)
{
    (void)l; (void)level; (void)module; (void)file;
    (void)line; (void)func; (void)fmt;
    if (used >= 3) abort();
    return results[used++];
}
int main(void)
{
    uint64_t calls[3] = {0}, late[3] = {0};
    int64_t status[3] = {0};
    struct phase p = { .payload = "x", .rate = 1000000, .count = 3,
        .threads = 1, .number = 1, .ready = 1, .calls = calls, .late = late,
        .status = status, .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER };
    struct producer producer = { .phase = &p };
    p.start = now_ns();
    p.end = p.start + 5000000000ull;
    produce(&producer);
    if (used != 3 || producer.attempted != 3 || producer.error) return 2;
    for (unsigned i = 0; i < 3; ++i) {
        if (!calls[i] || status[i] != results[i]) {
            puts("producer_status_not_recorded");
            return 1;
        }
    }
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.mutex);
    puts("producer_status_recorded");
    return 0;
}
'''

class ProducerStatus(unittest.TestCase):
    def test_real_producer_and_lost_status_mutation(self):
        source = (ROOT / "tests/bench_fixed_rate.c").read_text()
        anchor = "p->status[i] = status;"
        self.assertEqual(source.count(anchor), 1)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        env = dict(os.environ, LC_ALL="C")
        version = subprocess.run(compiler + ["--version"], capture_output=True,
                                 text=True, timeout=20, env=env, check=True)
        print("FIXED_RATE_STATUS_COMPILER=" + version.stdout.splitlines()[0], flush=True)
        with tempfile.TemporaryDirectory(prefix="fixed-rate-status-") as temp:
            work = Path(temp)
            (work / "probe.c").write_text(PRODUCER_FIXTURE)
            for lost in (False, True):
                # 只在临时副本变异；生产驱动和构建目标不被改写。
                (work / "driver.c").write_text(source.replace(anchor, "p->status[i] = 0; (void)status;") if lost else source)
                cmd = compiler + ["-std=gnu11", "-O0", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                  "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                  "-I" + str(ROOT / "include"), str(work / "probe.c"),
                                  "-pthread", "-o", str(work / "probe")]
                built = subprocess.run(cmd, capture_output=True, text=True, timeout=20, env=env)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([str(work / "probe")], capture_output=True, text=True,
                                        timeout=10, env=env)
                self.assertEqual(result.returncode, 1 if lost else 0, result.stderr)
                self.assertEqual(result.stdout, "producer_status_not_recorded\n" if lost else "producer_status_recorded\n")
                self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
