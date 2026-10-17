#!/usr/bin/env python3
"""宿主 FILE 非干扰回执判定器的有限正反例。"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "host_file_runner", ROOT / "scripts" / "check_fork_host_file.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def output(mode="held", fd=1, parent=2, child=2):
    return (f"FORK_HOST_FILE_PASS={mode} foreign_fd_open={fd} "
            f"parent_records={parent} child_records={child}\n")


class EvidenceTests(unittest.TestCase):
    def test_both_real_modes(self):
        for mode in RUNNER.MODES:
            result = RUNNER.verify(mode, 0, output(mode), "")
            self.assertEqual(result["parent_records"], 2)

    def test_skip_77_is_never_success(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUNNER.verify("held", 77, output(), "")

    def test_embedded_skip_is_never_success(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUNNER.verify("held", 0, output() + "FORK_SKIP=YES\n", "")

    def test_wrong_child_lines(self):
        with self.assertRaisesRegex(ValueError, "invalid_record"):
            RUNNER.verify("held", 0, output(child=1), "")

    def test_foreign_fd_was_closed(self):
        with self.assertRaisesRegex(ValueError, "invalid_record"):
            RUNNER.verify("held", 0, output(fd=0), "")

    def test_wrong_parent_lines(self):
        with self.assertRaisesRegex(ValueError, "invalid_record"):
            RUNNER.verify("held", 0, output(parent=3), "")

    def test_wrong_mode(self):
        with self.assertRaisesRegex(ValueError, "invalid_record"):
            RUNNER.verify("quiesced", 0, output("held"), "")

    def test_missing_or_duplicate_marker(self):
        with self.assertRaisesRegex(ValueError, "missing_or_duplicate"):
            RUNNER.verify("held", 0, "", "")
        with self.assertRaisesRegex(ValueError, "missing_or_duplicate"):
            RUNNER.verify("held", 0, output() * 2, "")

    def test_negative_sanitizer_warning(self):
        with self.assertRaisesRegex(ValueError, "sanitizer_diagnostic"):
            RUNNER.verify("held", 0, output(),
                          "ERROR: AddressSanitizer: use-after-free")

    def test_nonzero_exit(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUNNER.verify("held", 1, output(), "")


if __name__ == "__main__":
    unittest.main()
