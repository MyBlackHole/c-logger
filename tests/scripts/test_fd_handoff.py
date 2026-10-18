#!/usr/bin/env python3
"""移交语义的健康/错误进程判定；由 Xmake 提供已构建的三个目标。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

EXPECT = {
    0: (0, "ok"),
    1: (41, "output_source_not_consumed"),
    2: (42, "install_source_not_consumed"),
}


def expected_stdout(variant):
    return (f"FD_HANDOFF_VARIANT={variant}\n"
            f"FD_HANDOFF_RESULT={EXPECT[variant][1]}\n")


def validate(variant, completed):
    if (completed.returncode != EXPECT[variant][0] or
            completed.stdout != expected_stdout(variant) or completed.stderr):
        raise ValueError("handoff_result_mismatch")


def run_variant(variant, binary):
    executable = Path(binary).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="logger-handoff-") as directory:
        completed = subprocess.run([str(executable)], cwd=directory,
                                   capture_output=True, text=True,
                                   timeout=10, check=False)
    validate(variant, completed)
    return {"variant": variant, "exit_code": completed.returncode,
            "stdout": completed.stdout, "stderr": completed.stderr,
            "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}


class VerifierTests(unittest.TestCase):
    def completed(self, variant, **changes):
        values = {"returncode": EXPECT[variant][0],
                  "stdout": expected_stdout(variant), "stderr": ""}
        values.update(changes)
        return subprocess.CompletedProcess(["fixture"], **values)

    def test_exact_controls(self):
        for variant in EXPECT:
            with self.subTest(variant=variant):
                validate(variant, self.completed(variant))

    def test_unrelated_exits_are_not_mutation_success(self):
        for variant in EXPECT:
            for code in (1, 77, 124, -11, 66):
                with self.subTest(variant=variant, code=code):
                    with self.assertRaises(ValueError):
                        validate(variant, self.completed(variant, returncode=code))

    def test_healthy_binary_cannot_replace_a_mutant(self):
        for variant in (1, 2):
            with self.subTest(variant=variant), self.assertRaises(ValueError):
                validate(variant, self.completed(0))

    def test_mutants_cannot_replace_healthy_or_each_other(self):
        for target, actual in ((0, 1), (0, 2), (1, 2), (2, 1)):
            with self.subTest(target=target, actual=actual):
                with self.assertRaises(ValueError):
                    validate(target, self.completed(actual))

    def test_markers_require_exact_identity(self):
        for variant in EXPECT:
            for output in ("", expected_stdout(variant) * 2,
                           expected_stdout(variant) + "extra\n",
                           expected_stdout(variant).replace("RESULT", "OTHER")):
                with self.subTest(variant=variant, output=output):
                    with self.assertRaises(ValueError):
                        validate(variant, self.completed(variant, stdout=output))

    def test_diagnostics_are_never_ignored(self):
        for variant in EXPECT:
            for diagnostic in ("AddressSanitizer: error\n", "ThreadSanitizer\n",
                               "runtime error: undefined behavior\n", "other\n"):
                with self.subTest(variant=variant, diagnostic=diagnostic):
                    with self.assertRaises(ValueError):
                        validate(variant, self.completed(variant, stderr=diagnostic))

    def test_timeout_is_failure_not_expected_rejection(self):
        with patch("subprocess.run", side_effect=subprocess.TimeoutExpired("test", 10)):
            with self.assertRaises(subprocess.TimeoutExpired):
                run_variant(1, __file__)

    def test_missing_binary_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(OSError):
                run_variant(0, Path(directory) / "missing")


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(VerifierTests)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        return 1
    # 内部机器入口：无参数仅运行判定器自测；三个位置参数由 Xmake 提供。
    binaries = sys.argv[1:]
    if not binaries:
        return 0
    if len(binaries) != 3:
        print(json.dumps({"passed": False, "error": "three_binaries_required"}),
              file=sys.stderr)
        return 1
    try:
        results = [run_variant(variant, binary)
                   for variant, binary in enumerate(binaries)]
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"passed": False, "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"passed": True, "results": results}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
