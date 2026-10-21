#!/usr/bin/env python3
"""故障登记只接受可追溯的 fuzz 结果，并能闭合演练议题。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
REGISTER = ROOT / "scripts" / "register_periodic_fuzz_failure.py"
SOURCE = "a" * 40
FAKE_GH = """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
log = Path(os.environ['GH_FAKE_LOG'])
with log.open('a') as output:
    output.write(json.dumps(sys.argv[1:]) + '\\n')
if sys.argv[1:3] == ['issue', 'create']:
    print('https://github.com/MyBlackHole/c-logger/issues/999')
"""


class RegisterFailure(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        gh = bin_dir / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(0o755)
        self.env = os.environ.copy()
        self.env["PATH"] = str(bin_dir) + os.pathsep + self.env["PATH"]
        self.env["GH_FAKE_LOG"] = str(self.root / "gh.log")
        self.env["TMPDIR"] = str(self.root)
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()

    def result(self, status):
        (self.evidence / "result.json").write_text(json.dumps({
            "status": status, "source_commit": SOURCE,
            "raw_sha256": "b" * 64, "minimized_sha256": "c" * 64,
            "run_exit": 1, "replay_exit": 1,
        }))

    def invoke(self, *extra):
        return subprocess.run([
            sys.executable, str(REGISTER), "--repo", "MyBlackHole/c-logger",
            "--target", "public", "--evidence", str(self.evidence),
            "--source", SOURCE,
            "--run-url", "https://github.com/MyBlackHole/c-logger/actions/runs/123",
            *extra,
        ], cwd=ROOT, env=self.env, capture_output=True, text=True)

    def test_real_failure_opens_issue_with_sample_identity(self):
        self.result("failure_reproduced")
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in
                 (self.root / "gh.log").read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][:2], ["issue", "create"])
        body = (self.evidence / "registration.md").read_text()
        self.assertIn("c" * 64, body)
        self.assertIn("actions/runs/123", body)
        self.assertIn("关联 #212", body)

    def test_drill_creates_and_closes_record_without_claiming_product_bug(self):
        self.result("drill_pass")
        result = self.invoke("--drill")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in
                 (self.root / "gh.log").read_text().splitlines()]
        self.assertEqual([call[:2] for call in calls],
                         [["issue", "create"], ["issue", "close"]])
        self.assertIn("演练", (self.evidence / "registration.md").read_text())

    def test_passing_run_cannot_open_defect_issue(self):
        self.result("pass")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "gh.log").exists())


if __name__ == "__main__":
    unittest.main()
