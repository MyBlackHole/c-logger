#!/usr/bin/env python3
"""周期 fuzz 运行器的结果与失败证据契约。"""
import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "periodic_fuzz_runner.py"

FAKE_FUZZER = """#!/usr/bin/env python3
import os
from pathlib import Path
import sys

args = sys.argv[1:]
value = lambda name: next((a.split('=', 1)[1] for a in args if a.startswith(name + '=')), None)
if '-minimize_crash=1' in args:
    Path(value('-exact_artifact_path')).write_bytes(b'X')
    print('minimized one byte')
    raise SystemExit(0)
if '-runs=1' in args:
    if Path(args[-1]).read_bytes() == b'X':
        print('runtime error: signed integer overflow')
        raise SystemExit(1)
    raise SystemExit(0)
if os.environ.get('FAKE_FAIL') == '1':
    Path(value('-artifact_prefix') + 'crash-test').write_bytes(b'XX')
    print('runtime error: signed integer overflow')
    raise SystemExit(1)
corpus = Path(args[-1])
(corpus / 'copy').write_bytes(b'A')
(corpus / 'new').write_bytes(b'B')
print('normal fuzz completed')
"""


class PeriodicFuzzRunner(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / "fake-fuzzer"
        self.binary.write_text(FAKE_FUZZER)
        self.binary.chmod(0o755)
        self.library = self.root / "liblogger.a"
        self.library.write_bytes(b"test-library")
        self.seeds = self.root / "seeds"
        self.seeds.mkdir()
        (self.seeds / "initial").write_bytes(b"A")
        self.evidence = self.root / "evidence"

    def run_runner(self, *extra, fail=False, include_library=True):
        env = os.environ.copy()
        env["TMPDIR"] = str(self.root)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["FAKE_FAIL"] = "1" if fail else "0"
        command = [
            sys.executable, str(RUNNER), "--binary", str(self.binary),
            "--seed-corpus", str(self.seeds),
            "--evidence", str(self.evidence), "--seed", "107",
            "--seconds", "2", "--max-len", "128", "--input-timeout", "1",
            "--rss-mb", "256", *extra,
        ]
        if include_library:
            command.extend(("--library", str(self.library)))
        return subprocess.run(command, cwd=ROOT, env=env,
                              capture_output=True, text=True)

    def test_normal_run_records_bounded_parameters_and_deduplicates_corpus(self):
        result = self.run_runner()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["seed"], 107)
        self.assertEqual(report["run_exit"], 0)
        self.assertEqual(len(report["corpus"]), 2)
        self.assertEqual(sorted(p.read_bytes() for p in
                                (self.evidence / "dedup-corpus").iterdir()),
                         [b"A", b"B"])
        self.assertIn("-max_total_time=2", report["command"])
        self.assertIn("-rss_limit_mb=256", report["command"])

    def test_failure_preserves_sample_minimizes_and_replays(self):
        result = self.run_runner(fail=True)
        self.assertNotEqual(result.returncode, 0)
        report = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(report["status"], "failure_reproduced")
        self.assertEqual(report["run_exit"], 1)
        self.assertEqual(report["replay_exit"], 1)
        self.assertEqual((self.evidence / "raw.input").read_bytes(), b"XX")
        self.assertEqual((self.evidence / "minimized.input").read_bytes(), b"X")
        self.assertIn("signed integer overflow",
                      (self.evidence / "replay.log").read_text())

    def test_expected_drill_fails_if_diagnostic_is_missing(self):
        sample = self.root / "drill.input"
        sample.write_bytes(b"XX")
        result = self.run_runner("--drill-input", str(sample),
                                 "--expected-diagnostic", "AddressSanitizer")
        self.assertNotEqual(result.returncode, 0)
        report = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(report["status"], "drill_failed")

    def test_expected_drill_accepts_reproduced_sanitizer_failure(self):
        sample = self.root / "drill.input"
        sample.write_bytes(b"X")
        result = self.run_runner("--drill-input", str(sample),
                                 "--expected-diagnostic", "signed integer overflow",
                                 include_library=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads((self.evidence / "result.json").read_text())
        self.assertEqual(report["status"], "drill_pass")
        self.assertEqual(report["run_exit"], 1)
        self.assertEqual(report["replay_exit"], 1)
        self.assertIsNone(report["library_sha256"])

    def test_budget_above_limit_is_rejected_without_running(self):
        result = self.run_runner("--seconds", "181")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("seconds", result.stderr)
        self.assertFalse((self.evidence / "result.json").exists())

    def test_cpu_budget_stops_a_spinning_child(self):
        spec = importlib.util.spec_from_file_location("periodic_runner", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        spinner = self.root / "spinner.py"
        spinner.write_text("while True:\n    pass\n")
        result = runner.run([sys.executable, str(spinner)],
                            self.root / "spin.log", self.root,
                            os.environ.copy(), 5, cpu_seconds=1)
        self.assertNotEqual(result, 0)
        self.assertIn(result, (-24, -9))


if __name__ == "__main__":
    unittest.main()
