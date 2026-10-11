#!/usr/bin/env python3
"""校验 shell 国际化边界；完整双语检查需要 GNU gettext、envsubst、msgfmt。"""
import gettext
import locale
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "locales/zh_CN/LC_MESSAGES/c-logger-shell"
USAGE = "scripts/check.sh {fast|production|crash|unit|integration|concurrency|reliability|security|host-owned}"
RUNTIME = shutil.which("gettext") and shutil.which("envsubst")


class ShellMessages(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        xmake = self.bin / "xmake"
        xmake.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        xmake.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        LOGGER_PROJECT_VERSION="2.3.4", BUILD_SHARED="n", BUILD_DIR="build",
                        MODE="release", JOBS="1", LC_ALL="C", LANGUAGE="zh_CN")

    def run_script(self, name, *args, **env):
        return subprocess.run(["/bin/sh", str(ROOT / "scripts" / name), *args], cwd=ROOT,
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_english_and_exit_codes(self):
        for profile in ("fast", "production", "crash", "unit", "integration", "concurrency",
                        "reliability", "security", "host-owned", "invalid"):
            result = self.run_script("check.sh", profile)
            self.assertEqual(result.returncode, 2 if profile == "invalid" else 0)
            if profile == "invalid":
                self.assertEqual(result.stderr, "usage: " + USAGE + "\n")
            elif profile not in ("fast", "production", "crash"):
                self.assertEqual(result.stderr, f"profile '{profile}' is a compatibility alias; running the complete Xmake suite\n")
        result = self.run_script("check.sh", "fast", BUILD_SHARED="invalid")
        self.assertEqual((result.returncode, result.stderr), (2, "BUILD_SHARED must be y or n\n"))

    def test_missing_runtime_uses_english(self):
        # 隔离 PATH，确保系统即使安装 gettext 也能覆盖缺依赖分支。
        for command in ("dirname", "mkdir", "python3"):
            (self.bin / command).symlink_to(shutil.which(command))
        result = self.run_script("check.sh", "unit", PATH=str(self.bin), LC_ALL="zh_CN.UTF-8")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "profile 'unit' is a compatibility alias; running the complete Xmake suite\n")
        result = self.run_script("benchmark.sh", PATH=str(self.bin), LC_ALL="zh_CN.UTF-8",
                                 BENCH_BIN=str(self.work / "missing"), BENCH_OUT=str(self.work / "out"))
        self.assertEqual((result.returncode, result.stderr),
                         (2, "bench_matrix not found; build it with: xmake -j4 bench_matrix\n"))

    @unittest.skipUnless(RUNTIME, "GNU gettext/envsubst runtime is optional")
    def test_language_selection_and_machine_stdout(self):
        for locale, language, chinese in (("zh_CN.UTF-8", "", True), ("en_US.UTF-8", "zh_CN", True),
                                          ("en_US.UTF-8", "xx:zh_CN", True), ("en_US.UTF-8", "en", False),
                                          ("C", "zh_CN", False), ("C.UTF-8", "zh_CN", False),
                                          ("POSIX", "zh_CN", False), ("en_US.UTF-8", "xx", False)):
            for profile in ("fast", "production", "crash", "unit", "integration", "concurrency",
                            "reliability", "security", "host-owned", "invalid"):
                with self.subTest(locale=locale, language=language, profile=profile):
                    english = self.run_script("check.sh", profile)
                    result = self.run_script("check.sh", profile, LC_ALL=locale, LANGUAGE=language)
                    self.assertEqual((result.returncode, result.stdout), (english.returncode, english.stdout))
                    expected = english.stderr
                    if chinese:
                        expected = expected.replace("usage: ", "用法：").replace(
                            f"profile '{profile}' is a compatibility alias; running the complete Xmake suite",
                            f"配置 '{profile}' 是兼容别名；将运行完整 Xmake 测试集")
                    self.assertEqual(result.stderr, expected)
            invalid = self.run_script("check.sh", "fast", BUILD_SHARED="invalid", LC_ALL=locale, LANGUAGE=language)
            self.assertEqual(invalid.stderr, ("BUILD_SHARED 必须为 y 或 n" if chinese else "BUILD_SHARED must be y or n") + "\n")

    def test_benchmark_protocol(self):
        import json
        source = (ROOT / "scripts/benchmark.sh").read_text()
        keys = source.split('echo "', 1)[1].split('"', 1)[0].split(',')
        binary = self.bin / "bench"
        binary.write_text('#!/bin/sh\nprintf \'%s\\n\' \'' + json.dumps(dict.fromkeys(keys, 1)) + "'\n")
        binary.chmod(0o755)
        output = self.work / "result % $(touch SHOULD_NOT_EXIST)"
        for exists in (True, False):
            env = dict(BENCH_BIN=str(binary if exists else self.work / "missing"), BENCH_OUT=str(output))
            english = self.run_script("benchmark.sh", **env)
            csv = (output / "results.csv").read_bytes()
            jsonl = (output / "results.jsonl").read_bytes()
            self.assertEqual(english.returncode, 0 if exists else 2)
            if RUNTIME:
                chinese = self.run_script("benchmark.sh", LC_ALL="zh_CN.UTF-8", LANGUAGE="", **env)
                self.assertEqual(chinese.returncode, english.returncode)
                self.assertEqual((output / "results.csv").read_bytes(), csv)
                self.assertEqual((output / "results.jsonl").read_bytes(), jsonl)
                if exists:
                    self.assertEqual(chinese.stdout, "." * 28 + f"\n已写入 {output}/results.csv 和 {output}/results.jsonl\n")
                else:
                    self.assertEqual(chinese.stderr, "未找到 bench_matrix；请使用以下命令构建：xmake -j4 bench_matrix\n")
            if exists:
                self.assertEqual(len(csv.splitlines()), 29)
                self.assertEqual(len(jsonl.splitlines()), 28)
        self.assertFalse((ROOT / "SHOULD_NOT_EXIST").exists())

    @unittest.skipUnless(RUNTIME, "GNU gettext/envsubst runtime is optional")
    def test_missing_catalog_and_literal_translation(self):
        copy = self.work / "copy"
        shutil.copytree(ROOT / "scripts", copy / "scripts")
        result = subprocess.run(["/bin/sh", str(copy / "scripts/check.sh"), "unit"], cwd=copy,
                                env=dict(self.env, LC_ALL="zh_CN.UTF-8", LANGUAGE=""), capture_output=True, text=True)
        self.assertEqual(result.stderr, "profile 'unit' is a compatibility alias; running the complete Xmake suite\n")
        compiler = shutil.which("msgfmt")
        if not compiler:
            self.skipTest("msgfmt is required for a modified catalog fixture")
        catalog = copy / "locales/zh_CN/LC_MESSAGES"
        catalog.mkdir(parents=True)
        po = catalog / "c-logger-shell.po"
        po.write_text(CATALOG.with_suffix(".po").read_text().replace(
            "配置 '$profile' 是兼容别名；将运行完整 Xmake 测试集", "$(touch marker) `$profile` %s"))
        rejected = subprocess.run([compiler, "--check-format", "-o", os.devnull, str(po)], capture_output=True)
        self.assertNotEqual(rejected.returncode, 0)
        # 模拟绕过发布校验的损坏翻译；运行时仍只能输出文本，不能执行命令。
        subprocess.run([compiler, "-o", str(po.with_suffix(".mo")), str(po)], check=True)
        result = subprocess.run(["/bin/sh", str(copy / "scripts/check.sh"), "unit"], cwd=copy,
                                env=dict(self.env, LC_ALL="zh_CN.UTF-8", LANGUAGE=""), capture_output=True, text=True)
        self.assertEqual(result.stderr, "$(touch marker) `unit` %s\n")
        self.assertFalse((copy / "marker").exists())

    def test_catalog(self):
        with CATALOG.with_suffix(".mo").open("rb") as handle:
            translation = gettext.GNUTranslations(handle)
        self.assertEqual(translation.gettext("wrote $csv and $json"), "已写入 $csv 和 $json")
        compiler = shutil.which("msgfmt")
        if compiler:
            subprocess.run([compiler, "--check", "--check-format", "--no-hash", "-o", str(self.work / "messages.mo"),
                            str(CATALOG.with_suffix(".po"))], check=True)
            self.assertEqual((self.work / "messages.mo").read_bytes(), CATALOG.with_suffix(".mo").read_bytes())
            for translation in ("已写入 $csv", "已写入 $csv 和 $json $extra", "已写入 $csv 和 $other"):
                with self.subTest(invalid_translation=translation):
                    broken = self.work / "broken.po"
                    broken.write_text(CATALOG.with_suffix(".po").read_text().replace("已写入 $csv 和 $json", translation))
                    failure = subprocess.run([compiler, "--check", "--check-format", "--no-hash", "-o", os.devnull,
                                              str(broken)], capture_output=True)
                    self.assertNotEqual(failure.returncode, 0)


if __name__ == "__main__":
    if "--require-gettext" in sys.argv:
        sys.argv.remove("--require-gettext")
        for program in ("gettext", "envsubst", "msgfmt"):
            if not shutil.which(program):
                raise SystemExit("required GNU gettext tool is missing: " + program)
        original = locale.setlocale(locale.LC_ALL)
        try:
            for name in ("en_US.UTF-8", "zh_CN.UTF-8", "C.UTF-8"):
                locale.setlocale(locale.LC_ALL, name)
        except locale.Error as error:
            raise SystemExit("required UTF-8 locale is missing: " + str(error)) from error
        finally:
            locale.setlocale(locale.LC_ALL, original)
    unittest.main()
