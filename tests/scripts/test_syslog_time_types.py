#!/usr/bin/env python3
"""用真实 C 编译器验证 Syslog 时刻/时长边界；反例均有仅修正类型的正例。"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PREAMBLE = '''#include "logger_syslog.h"
int probe(logger_syslog_t *syslog, struct logger_monotonic_ms now,
          struct logger_duration_ms interval, uint64_t raw)
{
    (void)syslog;
    (void)now;
    (void)interval;
    (void)raw;
'''

# 片段只做语法/类型检查，不运行占位对象。每对仅替换一个值的类型。
CASES = {
    "duration_as_instant": (
        "syslog->retry_from_ms = interval; return 0;",
        "syslog->retry_from_ms = now; return 0;",
        ("logger_monotonic_ms", "logger_duration_ms")),
    "instant_as_duration": (
        "syslog->reconnect_interval_ms = now; return 0;",
        "syslog->reconnect_interval_ms = interval; return 0;",
        ("logger_monotonic_ms", "logger_duration_ms")),
    "raw_as_instant": (
        "syslog->retry_from_ms = raw; return 0;",
        "syslog->retry_from_ms = now; return 0;",
        ("logger_monotonic_ms",)),
    "raw_as_duration": (
        "syslog->reconnect_interval_ms = raw; return 0;",
        "syslog->reconnect_interval_ms = interval; return 0;",
        ("logger_duration_ms",)),
    "raw_before_argument": (
        "return logger_monotonic_ms_before(raw, now);",
        "return logger_monotonic_ms_before(now, now);",
        ("logger_monotonic_ms",)),
    "raw_elapsed_interval": (
        "return logger_monotonic_ms_elapsed(now, syslog->retry_from_ms, raw);",
        "return logger_monotonic_ms_elapsed(now, syslog->retry_from_ms, interval);",
        ("logger_duration_ms",)),
    "instant_elapsed_interval": (
        "return logger_monotonic_ms_elapsed(now, syslog->retry_from_ms, now);",
        "return logger_monotonic_ms_elapsed(now, syslog->retry_from_ms, interval);",
        ("logger_monotonic_ms", "logger_duration_ms")),
    "duration_elapsed_instant": (
        "return logger_monotonic_ms_elapsed(interval, syslog->retry_from_ms, interval);",
        "return logger_monotonic_ms_elapsed(now, syslog->retry_from_ms, interval);",
        ("logger_monotonic_ms", "logger_duration_ms")),
}

# 包含真实实现，以验证 static 输出槽签名，而不是另写一个相似声明。
CLOCK_PREAMBLE = '''#include "logger_syslog.c"
int probe(struct logger_monotonic_ms *instant,
          struct logger_duration_ms *duration, uint64_t *raw)
{
    (void)instant;
    (void)duration;
    (void)raw;
'''


class SyslogTimeTypeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Xmake 必须传入真实目标编译器；缺失配置或工具不能冒充反例通过。
        cls.compiler = shlex.split(os.environ.get("LOGGER_TEST_CC", ""))
        if not cls.compiler:
            raise ValueError("LOGGER_TEST_CC")
        cls.env = dict(os.environ, LC_ALL="C")
        result = subprocess.run(cls.compiler + ["--version"],
                                capture_output=True, text=True,
                                timeout=20, env=cls.env, check=True)
        print("SYSLOG_TIME_TYPE_COMPILER=" + result.stdout.splitlines()[0],
              flush=True)

    def compile(self, source):
        with tempfile.TemporaryDirectory(prefix="logger-syslog-time-type-") as work:
            path = Path(work) / "syslog_time_case.c"
            path.write_text(source, encoding="utf-8")
            command = self.compiler + [
                "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-fdiagnostics-color=never", "-fsyntax-only",
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"),
                str(path),
            ]
            return subprocess.run(command, capture_output=True, text=True,
                                  timeout=20, env=self.env)

    def check_pair(self, name, negative, positive, error_line, types):
        accepted = self.compile(positive)
        self.assertEqual(0, accepted.returncode, accepted.stderr)
        rejected = self.compile(negative)
        self.assertGreater(rejected.returncode, 0, rejected.stderr)
        # 必须在被替换的表达式行出现类型错误。缺头文件、语法错、信号与超时
        # 均不能靠非零退出码混入通过；每对的生产正例还须先独立编译成功。
        diagnostics = re.finditer(
            r"(?m)^[^\n]*[/\\]syslog_time_case\.c:(\d+):(?:\d+:)?"
            r"\s*error:\s*([^\n]+)", rejected.stderr)
        matching = [item.group(2) for item in diagnostics
                    if int(item.group(1)) == error_line]
        self.assertTrue(any("incompatible" in item for item in matching),
                        rejected.stderr)
        for expected in types:
            self.assertIn(expected, rejected.stderr)
        print("SYSLOG_TIME_TYPE_PAIR_PASS=" + name, flush=True)

    def check_fragment(self, name, negative, positive, types,
                       preamble=PREAMBLE):
        self.check_pair(name, preamble + negative + "\n}\n",
                        preamble + positive + "\n}\n",
                        preamble.count("\n") + 1, types)

    def check_production_pair(self, name, pattern, replacement):
        source = (ROOT / "src" / "logger_syslog.c").read_text(encoding="utf-8")
        sites = list(re.finditer(pattern, source))
        self.assertEqual(1, len(sites), "production mutation site: " + name)
        start, end = sites[0].span("value")
        negative = source[:start] + replacement + source[end:]
        self.check_pair(name, negative, source, source[:start].count("\n") + 1,
                        ("logger_monotonic_ms", "logger_duration_ms"))

    def test_reject_clock_output_pointer(self):
        for output, types in (
                ("duration", ("logger_monotonic_ms", "logger_duration_ms")),
                ("raw", ("logger_monotonic_ms",))):
            with self.subTest(output=output):
                self.check_fragment("clock_output_" + output,
                                    "return monotonic_ms(" + output + ");",
                                    "return monotonic_ms(instant);", types,
                                    CLOCK_PREAMBLE)

    def test_reject_production_elapsed_instant(self):
        self.check_production_pair(
            "production_elapsed_instant",
            r"\blogger_monotonic_ms_elapsed\(\s*now,\s*s->retry_from_ms,\s*"
            r"(?P<value>s->reconnect_interval_ms)\s*\)",
            "s->retry_from_ms")

    def test_reject_production_cooldown_duration(self):
        self.check_production_pair(
            "production_cooldown_duration",
            r"\bs->retry_from_ms\s*=\s*(?P<value>now)"
            r"(?=\s*;\s*s->retry_clock_pending\s*=\s*0\s*;)",
            "s->reconnect_interval_ms")

    def test_accept_same_type_copy_and_explicit_value_escape(self):
        # C 仍允许同型复制及显式提取/重建裸值；此通过样例记录静态保证的边界。
        source = '''struct logger_monotonic_ms copy = now;
struct logger_duration_ms interval_copy = interval;
syslog->retry_from_ms = (struct logger_monotonic_ms){interval_copy.value};
syslog->reconnect_interval_ms = (struct logger_duration_ms){(uint32_t)copy.value};
return logger_monotonic_ms_elapsed(copy, syslog->retry_from_ms, interval_copy);
'''
        result = self.compile(PREAMBLE + source + "}\n")
        self.assertEqual(0, result.returncode, result.stderr)
        print("SYSLOG_TIME_TYPE_ESCAPE_PASS", flush=True)


for case in CASES:
    def test(self, name=case):
        negative, positive, types = CASES[name]
        self.check_fragment(name, negative, positive, types)
    setattr(SyslogTimeTypeContract, "test_reject_" + case, test)


if __name__ == "__main__":
    unittest.main()
