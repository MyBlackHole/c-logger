#!/usr/bin/env python3
"""用真实 Global 实现验证完整票据/代次边界；反例均配对有效表达式的正例。"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

# 类型和五个 static helper 均来自真实实现，不另造声明或协议模型。
# 片段只做语法/类型检查，不运行占位拥有者、锁或原子存储。
PREAMBLE = '''#include "logger_global.c"
int probe(struct global_ticket ticket, struct global_generation generation,
          uint64_t raw, struct global_ticket *ticket_output,
          struct global_generation *generation_output)
{
    (void)ticket;
    (void)generation;
    (void)raw;
    (void)ticket_output;
    (void)generation_output;
'''

CASES = {
    "generation_as_ticket": (
        "*ticket_output = generation; return 0;",
        "*ticket_output = ticket; return 0;",
        ("global_ticket", "global_generation")),
    "ticket_as_generation": (
        "*generation_output = ticket; return 0;",
        "*generation_output = generation; return 0;",
        ("global_ticket", "global_generation")),
    "generation_phase_argument": (
        "return phase_of(generation);",
        "return phase_of(ticket);",
        ("global_ticket", "global_generation")),
    "raw_phase_argument": (
        "return phase_of(raw);",
        "return phase_of(ticket);",
        ("global_ticket",)),
}


class GlobalTicketTypeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Xmake 必须传入真实目标编译器；缺失工具或环境不能算类型拒绝。
        cls.compiler = shlex.split(os.environ.get("LOGGER_TEST_CC", ""))
        if not cls.compiler:
            raise ValueError("LOGGER_TEST_CC")
        cls.env = dict(os.environ, LC_ALL="C")
        result = subprocess.run(cls.compiler + ["--version"],
                                capture_output=True, text=True,
                                timeout=20, env=cls.env, check=True)
        print("GLOBAL_TICKET_TYPE_COMPILER=" + result.stdout.splitlines()[0],
              flush=True)

    def compile(self, source):
        with tempfile.TemporaryDirectory(prefix="logger-global-ticket-type-") as work:
            path = Path(work) / "global_ticket_case.c"
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
        # 先通过真实正例，再要求替换行的 incompatible 错误及其说明包含
        # 相关类型。编译器崩溃、超时、缺头文件和别处诊断均不能冒充命中。
        diagnostics = list(re.finditer(
            r"(?m)^([^\n]*?):(\d+):(?:\d+:)?\s*error:\s*([^\n]+)",
            rejected.stderr))
        matching = []
        for index, diagnostic in enumerate(diagnostics):
            if (Path(diagnostic.group(1)).name != "global_ticket_case.c" or
                    int(diagnostic.group(2)) != error_line or
                    "incompatible" not in diagnostic.group(3)):
                continue
            end = (diagnostics[index + 1].start()
                   if index + 1 < len(diagnostics) else len(rejected.stderr))
            block = rejected.stderr[diagnostic.start():end]
            matching.append(all(re.search(r"\b" + re.escape(expected) + r"\b",
                                          block) for expected in types))
        self.assertTrue(any(matching), rejected.stderr)
        print("GLOBAL_TICKET_TYPE_PAIR_PASS=" + name, flush=True)

    def check_fragment(self, name, negative, positive, types):
        self.check_pair(name, PREAMBLE + negative + "\n}\n",
                        PREAMBLE + positive + "\n}\n",
                        PREAMBLE.count("\n") + 1, types)

    def check_production_pair(self, name, pattern, replacement):
        source = (ROOT / "src" / "logger_global.c").read_text(encoding="utf-8")
        sites = list(re.finditer(pattern, source))
        self.assertEqual(1, len(sites), "production mutation site: " + name)
        start, end = sites[0].span("value")
        negative = source[:start] + replacement + source[end:]
        self.check_pair(name, negative, source, source[:start].count("\n") + 1,
                        ("global_ticket", "global_generation"))

    def test_reject_production_generation_phase_error(self):
        self.check_production_pair(
            "production_generation_phase_error",
            r"\bphase_error\(\s*(?P<value>ticket)\s*\)",
            "generation_of(ticket)")

    def test_reject_production_generation_poison_ticket(self):
        self.check_production_pair(
            "production_generation_poison_ticket",
            r"\bwith_phase\(\s*(?P<value>ticket)\s*,\s*G_BROKEN\s*\)",
            "generation_of(ticket)")

    def test_reject_production_generation_rollback_ticket(self):
        self.check_production_pair(
            "production_generation_rollback_ticket",
            r"\brollback_ticket\(\s*(?P<value>updating)\s*,\s*"
            r"phase_of\(\s*current\s*\)\s*\)",
            "generation_of(updating)")

    def test_reject_production_generation_decoded_twice(self):
        self.check_production_pair(
            "production_generation_decoded_twice",
            r"\bstruct\s+global_generation\s+generation\s*=\s*"
            r"generation_of\(\s*(?P<value>current)\s*\)",
            "generation_of(current)")

    def test_accept_same_type_copy_and_explicit_atomic_escape(self):
        # 同型复制、显式取值重建和原始原子存储仍可编译；这些边界仍需审查。
        # strong/weak CAS 的 expected 都是 uint64_t 指针，不建立第二套原子协议。
        source = '''struct global_ticket ticket_copy = ticket;
struct global_generation generation_copy = generation;
ticket = (struct global_ticket){generation_copy.value};
generation = (struct global_generation){ticket_copy.value};
raw = generation.value;
atomic_store_explicit(&g_ticket, raw, memory_order_relaxed);
ticket_copy.value = atomic_load_explicit(&g_ticket, memory_order_relaxed);
int strong = atomic_compare_exchange_strong_explicit(
    &g_ticket, &ticket_copy.value, generation.value,
    memory_order_relaxed, memory_order_relaxed);
int weak = atomic_compare_exchange_weak_explicit(
    &g_ticket, &raw, ticket.value,
    memory_order_relaxed, memory_order_relaxed);
*ticket_output = ticket_copy;
*generation_output = generation_copy;
return strong + weak + phase_of(ticket);
'''
        result = self.compile(PREAMBLE + source + "}\n")
        self.assertEqual(0, result.returncode, result.stderr)
        print("GLOBAL_TICKET_TYPE_ESCAPE_PASS", flush=True)


for case in CASES:
    def test(self, name=case):
        negative, positive, types = CASES[name]
        self.check_fragment(name, negative, positive, types)
    setattr(GlobalTicketTypeContract, "test_reject_" + case, test)


if __name__ == "__main__":
    unittest.main()
