#!/usr/bin/env python3
"""宿主 FILE 非干扰回执判定器的有限正反例。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from unittest import mock
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

    def test_timeout_preserves_partial_output_and_terminal_receipt(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            binary = directory / "fake-binary"
            binary.write_bytes(b"fake")
            out = directory / "observations"
            expired = subprocess.TimeoutExpired(
                [str(binary), "held"], 50,
                output=b"partial stdout", stderr=b"partial stderr")
            with mock.patch.object(
                    RUNNER.subprocess, "run", side_effect=expired):
                with mock.patch(
                        "sys.argv",
                        ["check_fork_host_file.py", "--binary", str(binary),
                         "--output", str(out), "--repeats", "1"]):
                    self.assertEqual(RUNNER.main(), 1)
            self.assertEqual(
                (out / "00-held.stdout").read_bytes(), b"partial stdout")
            self.assertEqual(
                (out / "00-held.stderr").read_bytes(), b"partial stderr")
            report = json.loads((out / "results.json").read_text())
            self.assertEqual(len(report["runs"]), 1)
            self.assertIsNone(report["runs"][0]["exit_code"])
            self.assertTrue(report["runs"][0]["timed_out"])
            self.assertNotIn("verified", report["runs"][0])
            self.assertIn("timeout:held:0",
                          (out / "failure.txt").read_text())

    def test_nonzero_exit(self):
        with self.assertRaisesRegex(ValueError, "nonzero_or_skipped"):
            RUNNER.verify("held", 1, output(), "")


if __name__ == "__main__":
    unittest.main()
