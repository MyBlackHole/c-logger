#!/usr/bin/env python3
"""压力证据执行器正反例：缺失子日志/跳过/假成功均被拒绝。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from unittest import mock
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fork_worker_stress_runner", ROOT / "scripts" /
    "run_fork_worker_stress.py")
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


def sample(mode="serial", children=4, parent=32, child=136, progress=16):
    return (f"FORK_WORKER_STRESS_PASS={mode} children={children}"
            f" parent_records={parent} child_records={child}"
            f" parent_progress_checks={progress}\n")


class WorkerStressRunnerTest(unittest.TestCase):
    def test_both_modes_accepted(self):
        for mode in RUN.MODES:
            self.assertEqual(
                RUN.parse_result(mode, 0, sample(mode), "")["child_records"],
                136)

    def test_exit_77_rejected(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUN.parse_result("serial", 77, sample(), "")

    def test_skip_marker_rejected(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUN.parse_result("serial", 0, sample() + "FORK_SKIP=bad\n", "")

    def test_missing_marker_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing_or_duplicate"):
            RUN.parse_result("serial", 0, "", "")

    def test_duplicate_marker_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing_or_duplicate"):
            RUN.parse_result("serial", 0, sample() * 2, "")

    def test_wrong_mode_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            RUN.parse_result("parallel", 0, sample("serial"), "")

    def test_wrong_child_count_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            RUN.parse_result("serial", 0, sample(children=3), "")

    def test_lost_parent_records_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            RUN.parse_result("serial", 0, sample(parent=1), "")

    def test_lost_child_records_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            RUN.parse_result("serial", 0, sample(child=135), "")

    def test_missing_progress_proof_rejected(self):
        without_proof = sample().replace(" parent_progress_checks=16", "")
        with self.assertRaisesRegex(ValueError, "missing_or_duplicate"):
            RUN.parse_result("serial", 0, without_proof, "")

    def test_insufficient_progress_proof_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected"):
            RUN.parse_result("serial", 0, sample(progress=15), "")

    def test_timeout_preserves_partial_output_and_terminal_receipt(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            binary = directory / "fake-binary"
            binary.write_bytes(b"fake")
            out = directory / "observations"
            expired = subprocess.TimeoutExpired(
                [str(binary), "serial"], 90,
                output=b"partial stdout", stderr=b"partial stderr")
            with mock.patch.object(
                    RUN.subprocess, "run", side_effect=expired):
                with mock.patch(
                        "sys.argv",
                        ["run_fork_worker_stress.py", "--binary", str(binary),
                         "--out", str(out), "--repeats", "1"]):
                    self.assertEqual(RUN.main(), 1)
            self.assertEqual(
                (out / "00-serial.stdout").read_bytes(), b"partial stdout")
            self.assertEqual(
                (out / "00-serial.stderr").read_bytes(), b"partial stderr")
            report = json.loads((out / "results.json").read_text())
            self.assertEqual(len(report["results"]), 1)
            self.assertIsNone(report["results"][0]["exit_code"])
            self.assertTrue(report["results"][0]["timed_out"])
            self.assertNotIn("verified", report["results"][0])
            self.assertIn("timeout:serial:0",
                          (out / "failure.txt").read_text())

    def test_timeout_with_text_output_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            binary = directory / "fake-binary"
            binary.write_bytes(b"fake")
            out = directory / "observations"
            expired = subprocess.TimeoutExpired(
                [str(binary), "serial"], 90,
                output="中文输出", stderr="失败诊断")
            with mock.patch.object(
                    RUN.subprocess, "run", side_effect=expired):
                with mock.patch(
                        "sys.argv",
                        ["run_fork_worker_stress.py", "--binary", str(binary),
                         "--out", str(out), "--repeats", "1"]):
                    self.assertEqual(RUN.main(), 1)
            self.assertEqual(
                (out / "00-serial.stdout").read_text(), "中文输出")
            self.assertEqual(
                (out / "00-serial.stderr").read_text(), "失败诊断")
            report = json.loads((out / "results.json").read_text())
            self.assertTrue(report["results"][0]["timed_out"])
            self.assertNotIn("verified", report["results"][0])

    def test_sanitizer_report_rejected(self):
        with self.assertRaisesRegex(ValueError, "sanitizer"):
            RUN.parse_result("serial", 0, sample(),
                             "ERROR: AddressSanitizer: use-after-free")


if __name__ == "__main__":
    unittest.main()
