#!/usr/bin/env python3
"""检测器验收器的正反例；不把计数或错误退出码单独当作通过。"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from check_asan_runtime import detection_ok, fork_ok


def sample():
    return ("SANITIZER_ATFORK registrations=1 dropped=0\n"
            + "".join("FORK_PROBE_ITERATION={} status=0\n".format(i) for i in range(96))
            + "FORK_PROBE completed=96 timeout=0 failed=0\n")


class Qualifier(unittest.TestCase):
    def test_complete(self):
        self.assertTrue(fork_ok(0, sample()))

    def test_failed_exit(self):
        for code in (1, 37, 77, -9, 139):
            self.assertFalse(fork_ok(code, sample()))

    def test_missing_registration(self):
        self.assertFalse(fork_ok(0, sample().replace("registrations=1", "registrations=0")))

    def test_dropped_handler(self):
        self.assertFalse(fork_ok(0, sample().replace("dropped=0", "dropped=1")))

    def test_missing_iteration(self):
        self.assertFalse(fork_ok(0, sample().replace("FORK_PROBE_ITERATION=0 status=0\n", "")))

    def test_duplicate_iteration(self):
        self.assertFalse(fork_ok(0, sample().replace("ITERATION=1 ", "ITERATION=0 ")))

    def test_failed_child(self):
        self.assertFalse(fork_ok(0, sample().replace("ITERATION=2 status=0", "ITERATION=2 status=14")))

    def test_forged_summary(self):
        self.assertFalse(fork_ok(0, sample().replace("completed=96", "completed=95")))

    def test_detection_controls(self):
        self.assertTrue(detection_ok("healthy", 0, "SANITIZER_HEALTHY=1\n"))
        self.assertTrue(detection_ok("heap-overflow", 1, "ERROR: AddressSanitizer: heap-buffer-overflow"))
        self.assertTrue(detection_ok("signed-overflow", 1, "runtime error: signed integer overflow"))

    def test_no_instrumentation(self):
        for kind in ("heap-overflow", "signed-overflow"):
            self.assertFalse(detection_ok(kind, 0, "SANITIZER_MISSED_ERROR=1"))
            self.assertFalse(detection_ok(kind, 1, "unrelated error"))

    def test_crash_is_not_detection(self):
        self.assertFalse(detection_ok("heap-overflow", -11, "ERROR: AddressSanitizer: heap-buffer-overflow"))

    def test_recovered_undefined_behavior_rejected(self):
        self.assertFalse(detection_ok("signed-overflow", 0, "runtime error: signed integer overflow"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
