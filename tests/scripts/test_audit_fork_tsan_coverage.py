#!/usr/bin/env python3
"""TSan/ASan 覆盖审计的正反例；不依赖实际编译器或 CI 归档。"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fork_coverage", ROOT / "scripts" / "audit_fork_tsan_coverage.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
SHA = "a" * 40
TREE = "b" * 40


def suite(tsan):
    tests = sorted(AUDIT.EXPECTED_SKIPS | AUDIT.TSAN_ACTIVE |
                   {f"fixture_{i}/case" for i in range(510)})
    lines = []
    for name in tests:
        target, mode = name.split("/", 1)
        lines.append(f"[ 40%]: running.test {name}")
        if tsan and name in AUDIT.EXPECTED_SKIPS:
            reason = "FORK_REINIT_SKIP" if target == "fork_reinit_public_test" else "FORK_RUNTIME_SKIP"
            lines.append(f"{reason}=tsan-post-fork-worker case={mode}")
            lines.append(f"FORK_RUNTIME_SKIP={target}/{target}/{mode}")
        elif name in AUDIT.EXPECTED_SKIPS | AUDIT.TSAN_ACTIVE:
            marker = ("FORK_REINIT_PUBLIC_PASS" if target == "fork_reinit_public_test"
                      else "FORK_RUNTIME_PASS")
            lines.append(f"{marker}={mode} child_async=0")
    for name in tests:
        lines.append(f"[ 40%]: {name} .... passed 0.002s")
    lines.append(f"100% tests passed, 0 test(s) failed out of {len(tests)}, spent 3.0s")
    return "\n".join(lines) + "\n"


def create_directory(root, name, kind, content=None):
    path = root / name
    path.mkdir()
    (path / "test-exit-code.txt").write_text("0\n")
    (path / "policies.txt").write_text(
        "build.sanitizer.thread\n" if kind == "tsan" else
        "build.sanitizer.address,build.sanitizer.undefined\n")
    (path / "source-commit.txt").write_text(SHA + "\n")
    (path / "source-tree.txt").write_text(TREE + "\n")
    (path / "xmake-tests.log").write_text(content if content is not None else
                                         suite(kind == "tsan"))
    return path


def create_model(root):
    root.mkdir()
    rows = []
    for mode, code, warning in (
            ("healthy", 0, None),
            ("order-error", 66, "WARNING: ThreadSanitizer: lock-order-inversion"),
            ("race-error", 66, "WARNING: ThreadSanitizer: data race")):
        rows.append({"case": mode, "exit_code": code,
                     "expected_exit": code, "accepted": True})
        text = "parent had 1 threads\n" + (warning or "") + "\n"
        (root / (mode + ".log")).write_text(text)
    import json
    (root / "results.json").write_text(json.dumps(rows))


class CoverageAudit(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.tsan = create_directory(root, "tsan", "tsan")
        self.asan = create_directory(root, "asan", "asan")
        self.model = root / "model"
        create_model(self.model)

    def tearDown(self):
        self.tmp.cleanup()

    def check(self):
        t = AUDIT.check_directory(self.tsan, "build.sanitizer.thread")
        a = AUDIT.check_directory(self.asan,
             "build.sanitizer.address,build.sanitizer.undefined")
        report = AUDIT.validate(t, a, SHA)
        report["model"] = AUDIT.validate_model(self.model)
        return report

    def test_valid_provenance(self):
        r = self.check()
        self.assertEqual(r["tsan_explicit_skip"], 31)
        self.assertEqual(r["tsan_sync_anchor_pass"], 9)
        self.assertEqual(r["asan_ubsan_actual_pass"], 550)
        self.assertEqual(r["tsan_actual_pass"], 519)

    def mutate(self, path, original, replacement):
        txt = path.read_text()
        self.assertIn(original, txt)
        path.write_text(txt.replace(original, replacement, 1))

    def test_missing_skip_cannot_pass(self):
        self.mutate(self.tsan / "xmake-tests.log",
                    "FORK_RUNTIME_SKIP=fork_runtime_regression/fork_runtime_regression/mapped\n",
                    "")
        with self.assertRaisesRegex(ValueError, "tsan_skip_count"):
            self.check()

    def test_changed_reason_cannot_pass(self):
        self.mutate(self.tsan / "xmake-tests.log",
                    "FORK_REINIT_SKIP=tsan-post-fork-worker case=async",
                    "FORK_REINIT_SKIP=other-reason case=async")
        with self.assertRaisesRegex(ValueError, "unknown_reinit_skip_reason"):
            self.check()

    def test_asan_without_real_execution_cannot_pass(self):
        self.mutate(self.asan / "xmake-tests.log",
                    "FORK_REINIT_PUBLIC_PASS=async child_async=0", "MISSING_ASAN_PASS")
        with self.assertRaisesRegex(ValueError, "asan_real_execution_marker_missing"):
            self.check()

    def test_other_source_tree_cannot_pass(self):
        (self.asan / "source-tree.txt").write_text("f" * 40 + "\n")
        with self.assertRaisesRegex(ValueError, "source_tree_mismatch"):
            self.check()

    def test_tsan_without_active_anchor_cannot_pass(self):
        self.mutate(self.tsan / "xmake-tests.log",
                    "FORK_RUNTIME_PASS=mapped-sync child_async=0", "MISSING_SYNC")
        with self.assertRaisesRegex(ValueError, "tsan_active_fork_anchor_missing"):
            self.check()

    def test_missing_model_control_cannot_pass(self):
        self.mutate(self.model / "race-error.log",
                    "WARNING: ThreadSanitizer: data race", "NO_RACE_DIAGNOSTIC")
        with self.assertRaisesRegex(ValueError, "tsan_model_did_not_detect_error"):
            self.check()

    def test_nonzero_suite_exit_cannot_pass(self):
        (self.asan / "test-exit-code.txt").write_text("1\n")
        with self.assertRaisesRegex(ValueError, "suite_exit_not_zero"):
            self.check()

    def test_wrong_skip_count_cannot_pass(self):
        self.mutate(self.tsan / "xmake-tests.log",
                    "FORK_RUNTIME_SKIP=fork_reinit_public_test/fork_reinit_public_test/global\n",
                    "FORK_RUNTIME_SKIP=fork_reinit_public_test/fork_reinit_public_test/async\n")
        with self.assertRaisesRegex(ValueError, "tsan_skip_count"):
            self.check()


if __name__ == "__main__":
    unittest.main()
