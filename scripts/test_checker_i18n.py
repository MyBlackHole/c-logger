#!/usr/bin/env python3
"""六检查工具的双语帮助、判定与机器证据不变性回归。"""
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

from check_crash_matrix import EXPECTED

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
CHECKERS = ("check_consumer_glibc", "check_crash_matrix", "check_production_artifact",
            "check_release_abi", "check_release_assets", "check_v2_diff")


class CheckerI18n(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="checker languages ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def cli(self, name, language, *args, cwd=None):
        return subprocess.run([sys.executable, str(SCRIPTS / (name + ".py")),
                               "--lang=" + language, *map(str, args)],
                              cwd=cwd or self.root, capture_output=True)

    def test_filtered_workflow_dependencies(self):
        workflows = ROOT / ".github/workflows"
        expected = {"core-validation": 1, "crash-recovery": 2, "glibc-runtime": 2,
                    "release-validation": 1, "xmake-parity": 2, "v2-engineering-baseline": 2}
        for name, count in expected.items():
            text = (workflows / (name + ".yml")).read_text()
            blocks = re.findall(r"(?ms)^  (?:pull_request|push):\n(.*?)(?=^  \w+:|^\S|\Z)", text)
            filtered = [block for block in blocks if "    paths:" in block]
            self.assertEqual(len(filtered), count, name)
            for block in filtered:
                self.assertIn('      - "locales/**"', block, name)
                self.assertTrue('      - "scripts/tool_i18n.py"' in block or
                                '      - "scripts/**"' in block, name)
        # 发布流程继续仅由标签/手动触发，不为语言检查增加发布入口。
        publish = (workflows / "release-publish.yml").read_text().split("\njobs:", 1)[0]
        self.assertNotIn("pull_request:", publish)
        self.assertNotIn("    paths:", publish)

    def test_all_help_and_usage_codes(self):
        for name in CHECKERS:
            with self.subTest(name=name):
                en, zh = [self.cli(name, lang, "--help") for lang in ("en", "zh_CN")]
                self.assertEqual((en.returncode, zh.returncode), (0, 0))
                self.assertIn(b"--lang", en.stdout)
                self.assertIn("人读".encode(), zh.stdout)
                self.assertNotEqual(en.stdout, zh.stdout)
                self.assertEqual(self.cli(name, "invalid").returncode, 2)

    def test_json_errors_are_byte_identical(self):
        for name, args in (("check_consumer_glibc", ["missing", "--max-glibc", "2.31"]),
                           ("check_release_abi", ["missing"])):
            en, zh = [self.cli(name, lang, *args) for lang in ("en", "zh_CN")]
            self.assertEqual((en.returncode, zh.returncode), (2, 2))
            self.assertEqual(en.stdout, zh.stdout)
            self.assertFalse(json.loads(en.stdout)["passed"])

    def test_real_elf_json_is_byte_identical(self):
        source = self.root / "fixture.c"
        source.write_text("int main(void) { return 0; }\n")
        executable = self.root / "fixture"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                       [str(source), "-o", str(executable)], check=True, capture_output=True)
        for name, args in (("check_consumer_glibc", [executable, "--max-glibc", "999.0"]),
                           ("check_production_artifact", [executable]),
                           ("check_release_abi", [executable])):
            en, zh = [self.cli(name, lang, *args) for lang in ("en", "zh_CN")]
            self.assertEqual(en.returncode, zh.returncode, en.stderr + zh.stderr)
            self.assertIn(en.returncode, (0, 1))
            self.assertEqual(en.stdout, zh.stdout)
            json.loads(en.stdout)

    def test_crash_json_and_junit_are_byte_identical(self):
        rows = [f"[100%]: {name} ........ passed 0.001s" for name in sorted(EXPECTED)]
        cases = [(rows, 0), (rows[:-1], 1), (rows + rows[:1], 1),
                 ([rows[0].replace("passed", "failed")] + rows[1:], 1),
                 (rows + ["[100%]: other/unexpected ... passed 0.001s"], 1), ([], 1)]
        log = self.root / "xmake.log"
        evidence, junit = self.root / "evidence.json", self.root / "junit"
        for lines, expected in cases:
            with self.subTest(lines=lines):
                log.write_text("\n".join(lines) + "\n")
                results = []
                for language in ("en", "zh_CN"):
                    run = self.cli("check_crash_matrix", language, log, "--output", evidence,
                                   "--junit-dir", junit)
                    self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
                    results.append((evidence.read_bytes(), (junit / "crash-junit-1.xml").read_bytes()))
                self.assertEqual(*results)

    def test_release_checksum_records_and_failures(self):
        for kind in ("shared", "static"):
            package = self.root / f"prod-c-logger-2.0.0-fixture-{kind}.tar.gz"
            package.write_bytes(b"checksum fixture")
            package.with_name(package.name + ".sha256").write_text(
                hashlib.sha256(package.read_bytes()).hexdigest() + "  " + package.name + "\n")
        args = ("--directory", self.root, "--version", "2.0.0")
        en, zh = [self.cli("check_release_assets", lang, *args) for lang in ("en", "zh_CN")]
        self.assertEqual((en.returncode, zh.returncode), (0, 0))
        self.assertEqual(en.stdout, zh.stdout)
        self.assertEqual(en.stdout.count(b"verified "), 2)
        for lang, message in (("en", b"Release asset verification failed"),
                              ("zh_CN", "发布包核验失败".encode())):
            result = self.cli("check_release_assets", lang, "--version", "invalid")
            self.assertEqual(result.returncode, 1)
            self.assertIn(message, result.stderr)

    def test_v2_pass_and_fail_codes(self):
        def git(*args):
            return subprocess.check_output(["git", *args], cwd=self.root).decode().strip()
        git("init", "-b", "main")
        git("config", "user.name", "国际化自测")
        git("config", "user.email", "fixture@example.invalid")
        (self.root / "fixture.c").write_text("int fixture;\n")
        git("add", ".")
        git("commit", "-qm", "初始自测")
        base = git("rev-parse", "HEAD")
        for lang in ("en", "zh_CN"):
            self.assertEqual(self.cli("check_v2_diff", lang, base, cwd=self.root).returncode, 0)
        (self.root / "fixture.c").write_text("strcpy(a, b);")
        git("add", ".")
        git("commit", "-qm", "违规自测")
        for lang, message in (("en", b"forbidden"), ("zh_CN", "禁止".encode())):
            result = self.cli("check_v2_diff", lang, base, cwd=self.root)
            self.assertEqual(result.returncode, 1)
            self.assertIn(message, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
