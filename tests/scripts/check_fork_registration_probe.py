#!/usr/bin/env python3
"""用真实测试进程验证 atfork 探针不会漏掉重复注册或误计外部注册。"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from tool_i18n import parser_for

SWITCHES = ("LOGGER_TEST_DUPLICATE_ATFORK", "LOGGER_TEST_OMIT_ATFORK_OBSERVATION")
SCENARIOS = (("healthy", None, 1), ("duplicate", SWITCHES[0], 2),
             ("unobserved", SWITCHES[1], 0))
ASSERTION = "atomic_load(&registrations) == 1"


def accepted(name, code, text):
    """非零退出本身不是成功捕获；必须命中本探针的精确拒绝点。"""
    expected = dict((label, count) for label, _, count in SCENARIOS)[name]
    if any(marker in text for marker in ("AddressSanitizer:", "UndefinedBehaviorSanitizer:",
                                         "ThreadSanitizer:", "runtime error:")):
        return False
    marker = "ATFORK_LOGGER_REGISTRATIONS=" + str(expected)
    if text.splitlines().count(marker) != 1:
        return False
    if name == "healthy":
        return code == 0 and ASSERTION not in text
    return code == 1 and ASSERTION in text


class OracleTests(unittest.TestCase):
    def test_healthy(self):
        self.assertTrue(accepted("healthy", 0, "ATFORK_LOGGER_REGISTRATIONS=1\n"))

    def test_duplicate(self):
        self.assertTrue(accepted("duplicate", 1, "ATFORK_LOGGER_REGISTRATIONS=2\n" + ASSERTION))

    def test_unobserved(self):
        self.assertTrue(accepted("unobserved", 1, "ATFORK_LOGGER_REGISTRATIONS=0\n" + ASSERTION))

    def test_arbitrary_nonzero(self):
        for code in (0, 2, 77, 124, -6, -11):
            self.assertFalse(accepted("duplicate", code, "ATFORK_LOGGER_REGISTRATIONS=2\n" + ASSERTION))

    def test_wrong_count(self):
        self.assertFalse(accepted("duplicate", 1, "ATFORK_LOGGER_REGISTRATIONS=1\n" + ASSERTION))

    def test_missing_assertion(self):
        self.assertFalse(accepted("duplicate", 1, "ATFORK_LOGGER_REGISTRATIONS=2\n"))

    def test_missing_counter(self):
        self.assertFalse(accepted("duplicate", 1, ASSERTION))

    def test_duplicate_counter(self):
        self.assertFalse(accepted("duplicate", 1, "ATFORK_LOGGER_REGISTRATIONS=2\n" * 2 + ASSERTION))

    def test_unrelated_sanitizer_failure(self):
        self.assertFalse(accepted("duplicate", 1, "ATFORK_LOGGER_REGISTRATIONS=2\n" + ASSERTION + "\nERROR: AddressSanitizer: bad-access"))

    def test_healthy_cannot_contain_assertion(self):
        self.assertFalse(accepted("healthy", 0, "ATFORK_LOGGER_REGISTRATIONS=1\n" + ASSERTION))


def verify(binary, output):
    binary = binary.resolve(strict=True)
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for name, switch, _ in SCENARIOS:
        env = os.environ.copy()
        for key in SWITCHES:
            env.pop(key, None)
        if switch:
            env[switch] = "1"
        command = [str(binary), "explicit"]
        with tempfile.TemporaryDirectory(prefix="logger-fork-probe-") as work:
            try:
                result = subprocess.run(command, cwd=work, env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=20, check=False)
                data, code = result.stdout, result.returncode
            except subprocess.TimeoutExpired as error:
                data, code = (error.stdout or b""), 124
        text = data.decode("utf-8", errors="replace")
        (output / (name + ".log")).write_bytes(data)
        records.append({"case": name, "command": command, "switch": switch,
                        "exit_code": code, "accepted": accepted(name, code, text),
                        "log_sha256": hashlib.sha256(data).hexdigest()})
    summary = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "cases": records, "passed": sum(item["accepted"] for item in records)}
    (output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["passed"] == len(SCENARIOS) else 1


def main():
    parser, _ = parser_for("Validate fork registration probe evidence.")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.selftest:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(OracleTests))
        return 0 if result.wasSuccessful() else 1
    if args.binary is None or args.output is None:
        parser.error(_("Both --binary and --output are required."))
    try:
        return verify(args.binary, args.output)
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error)}, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
