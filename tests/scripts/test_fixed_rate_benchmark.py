#!/usr/bin/env python3
"""固定速率验收器必须拒绝伪造计数、分位和缺失/重复/损坏输出。"""
import array
import copy
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
        self.phases = {1: (array.array("Q", [10, 20, 0]), array.array("Q", [0, 2000000, 0]))}
        self.row = {"phase": 1, "scheduled": 3, "attempted": 2, "generator_missed": 1,
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
                array.array("Q", [1, 3]).tofile(stream)
                for values in self.phases[1]:
                    values.tofile(stream)
            self.assertEqual(bench.read_raw(path), self.phases)
            path.write_bytes(b"bad")
            with self.assertRaises(ValueError):
                bench.read_raw(path)

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


if __name__ == "__main__":
    unittest.main()
