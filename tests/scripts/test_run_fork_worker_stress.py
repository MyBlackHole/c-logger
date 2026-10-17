#!/usr/bin/env python3
"""压力证据执行器正反例：缺失子日志/跳过/假成功均被拒绝。"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fork_worker_stress_runner", ROOT / "scripts" /
    "run_fork_worker_stress.py")
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


def sample(mode="serial", children=4, parent=32, child=136):
    return (f"FORK_WORKER_STRESS_PASS={mode} children={children}"
            f" parent_records={parent} child_records={child}\n")


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

    def test_sanitizer_report_rejected(self):
        with self.assertRaisesRegex(ValueError, "sanitizer"):
            RUN.parse_result("serial", 0, sample(),
                             "ERROR: AddressSanitizer: use-after-free")


if __name__ == "__main__":
    unittest.main()
