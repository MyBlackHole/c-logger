#!/usr/bin/env python3
"""以真实枚举 helper 和生产源码验证有限原子写入契约；不证明所有整数/转换逃逸。"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]

# 这些片段只检查类型，不执行未初始化的原子对象或占位参数。
# 数值重叠使反例只能依赖枚举类别，不能靠越界哨兵得到拒绝。
PREAMBLE = '''#define _GNU_SOURCE
#include "logger_internal.h"
_Static_assert((int)LOGGER_TRACE == (int)LOGGER_STATE_CREATED, "zero overlap");
_Static_assert((int)LOGGER_DEBUG == (int)LOGGER_STATE_RUNNING, "one overlap");
_Static_assert((int)LOGGER_INFO == (int)LOGGER_STATE_STOPPING, "two overlap");
_Static_assert((int)LOGGER_WARN == (int)LOGGER_STATE_STOPPED, "three overlap");
int probe(void)
{
    _Atomic logger_state_t state_target;
    _Atomic logger_level_t level_target;
    logger_state_t state = LOGGER_STATE_STOPPING;
    logger_level_t level = LOGGER_INFO;
    logger_state_t expected = LOGGER_STATE_RUNNING;
    logger_level_t wrong_expected = LOGGER_DEBUG;
    int raw = 2;
    (void)&state_target; (void)&level_target;
    (void)state; (void)level; (void)expected; (void)wrong_expected; (void)raw;
'''

# 反例、只修正类型的正例、诊断类别、诊断中必须出现的目标类型。
CASES = {
    "state_init_constant": (
        "logger_state_init(&state_target, LOGGER_TRACE);",
        "logger_state_init(&state_target, LOGGER_STATE_CREATED);",
        "enum-compare", "logger_state_t"),
    "state_init_variable": (
        "logger_state_init(&state_target, level);",
        "logger_state_init(&state_target, state);",
        "enum-compare", "logger_state_t"),
    "level_init_constant": (
        "logger_level_init(&level_target, LOGGER_STATE_CREATED);",
        "logger_level_init(&level_target, LOGGER_TRACE);",
        "enum-compare", "logger_level_t"),
    "level_init_variable": (
        "logger_level_init(&level_target, state);",
        "logger_level_init(&level_target, level);",
        "enum-compare", "logger_level_t"),
    "state_store_constant": (
        "logger_state_store_release(&state_target, LOGGER_INFO);",
        "logger_state_store_release(&state_target, LOGGER_STATE_STOPPING);",
        "enum-compare", "logger_state_t"),
    "state_store_variable": (
        "logger_state_store_release(&state_target, level);",
        "logger_state_store_release(&state_target, state);",
        "enum-compare", "logger_state_t"),
    "level_store_constant": (
        "logger_level_store_relaxed(&level_target, LOGGER_STATE_STOPPING);",
        "logger_level_store_relaxed(&level_target, LOGGER_INFO);",
        "enum-compare", "logger_level_t"),
    "level_store_variable": (
        "logger_level_store_relaxed(&level_target, state);",
        "logger_level_store_relaxed(&level_target, level);",
        "enum-compare", "logger_level_t"),
    "state_cas_desired_constant": (
        "(void)logger_state_try_publish(&state_target, &expected, LOGGER_DEBUG);",
        "(void)logger_state_try_publish(&state_target, &expected, LOGGER_STATE_RUNNING);",
        "enum-compare", "logger_state_t"),
    "state_cas_desired_variable": (
        "(void)logger_state_try_publish(&state_target, &expected, level);",
        "(void)logger_state_try_publish(&state_target, &expected, state);",
        "enum-compare", "logger_state_t"),
    "state_cas_expected_pointer": (
        "(void)logger_state_try_publish(&state_target, &wrong_expected, state);",
        "(void)logger_state_try_publish(&state_target, &expected, state);",
        "incompatible-pointer-types", "logger_state_t"),
    "state_cas_destination_pointer": (
        "(void)logger_state_try_publish(&level_target, &expected, state);",
        "(void)logger_state_try_publish(&state_target, &expected, state);",
        "incompatible-pointer-types", "logger_state_t"),
    "state_init_destination_pointer": (
        "logger_state_init(&level_target, state);",
        "logger_state_init(&state_target, state);",
        "incompatible-pointer-types", "logger_state_t"),
    "level_init_destination_pointer": (
        "logger_level_init(&state_target, level);",
        "logger_level_init(&level_target, level);",
        "incompatible-pointer-types", "logger_level_t"),
    "state_store_destination_pointer": (
        "logger_state_store_release(&level_target, state);",
        "logger_state_store_release(&state_target, state);",
        "incompatible-pointer-types", "logger_state_t"),
    "level_store_destination_pointer": (
        "logger_level_store_relaxed(&state_target, level);",
        "logger_level_store_relaxed(&level_target, level);",
        "incompatible-pointer-types", "logger_level_t"),
}

# 只替换既有写点的值实参；构造 level 使用已有的 CREATED=0 状态变量。
PRODUCTION_CASES = {
    "production_ctor_rollback_state": (
        "logger.c", "logger_ctor_rollback", "logger_state_store_release",
        "&l->state", "LOGGER_STATE_STOPPING", "LOGGER_INFO", "logger_state_t"),
    "production_create_level": (
        "logger.c", "create_logger", "logger_level_init",
        "&l->level", "cfg.level", "expected", "logger_level_t"),
    "production_create_state": (
        "logger.c", "create_logger", "logger_state_init",
        "&l->state", "LOGGER_STATE_CREATED", "LOGGER_TRACE", "logger_state_t"),
    "production_create_publish": (
        "logger.c", "create_logger", "logger_state_try_publish",
        "&l->state, &expected", "LOGGER_STATE_RUNNING", "LOGGER_DEBUG", "logger_state_t"),
    "production_dispose_stopping": (
        "logger.c", "dispose_body", "logger_state_store_release",
        "&l->state", "LOGGER_STATE_STOPPING", "LOGGER_INFO", "logger_state_t"),
    "production_dispose_stopped": (
        "logger.c", "dispose_body", "logger_state_store_release",
        "&l->state", "LOGGER_STATE_STOPPED", "LOGGER_WARN", "logger_state_t"),
    "production_lifecycle_failure": (
        "logger.c", "logger_note_lifecycle_error", "logger_state_store_release",
        "&l->state", "LOGGER_STATE_STOPPING", "LOGGER_INFO", "logger_state_t"),
    "production_instance_level": (
        "logger.c", "logger_set_instance_level", "logger_level_store_relaxed",
        "&l->level", "v", "LOGGER_STATE_STOPPING", "logger_level_t"),
    "production_global_level": (
        "logger_global.c", "logger_set_level", "logger_level_store_relaxed",
        "&g_logger->level", "level", "LOGGER_STATE_STOPPING", "logger_level_t"),
    "production_worker_failure": (
        "logger_worker.c", "logger_worker_main", "logger_state_store_release",
        "&l->state", "LOGGER_STATE_STOPPING", "LOGGER_INFO", "logger_state_t"),
}


class EnumAtomicTypeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 使用 Xmake 的真实目标编译器，不以工具缺失冒充类型拒绝。
        cls.compiler = shlex.split(os.environ.get("LOGGER_TEST_CC", ""))
        if not cls.compiler:
            raise ValueError("LOGGER_TEST_CC")
        cls.env = dict(os.environ, LC_ALL="C")
        result = subprocess.run(cls.compiler + ["--version"], capture_output=True,
                                text=True, timeout=20, env=cls.env, check=True)
        print("ENUM_ATOMIC_TYPE_COMPILER=" + result.stdout.splitlines()[0], flush=True)

    def compile(self, source):
        with tempfile.TemporaryDirectory(prefix="logger-enum-atomic-type-") as work:
            path = Path(work) / "enum_atomic_case.c"
            path.write_text(source, encoding="utf-8")
            command = self.compiler + [
                "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-fdiagnostics-color=never", "-fsyntax-only",
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"), str(path),
            ]
            return subprocess.run(command, capture_output=True, text=True,
                                  timeout=20, env=self.env)

    def check_pair(self, name, negative, positive, error_line, call_line,
                   helper, category, target_type):
        accepted = self.compile(positive)
        self.assertEqual(0, accepted.returncode, accepted.stderr)
        rejected = self.compile(negative)
        self.assertGreater(rejected.returncode, 0, rejected.stderr)
        # GCC 可在宏定义处报错，再以 note 指向调用；Clang 通常直接报调用处。
        # 只接纳同一主错误的展开链，并精确关联此实参/调用行、helper 和目标类型。
        # GCC 的匿名枚举常量未必打印来源 typedef，不能用别处错误冒充拒绝。
        # 旧 Clang 对直接常量实参报转换诊断；值检查只接纳这两类枚举诊断。
        categories = (("enum-compare", "enum-conversion")
                      if category == "enum-compare" else (category,))
        source_path = Path(rejected.args[-1])
        header_path = ROOT / "src/logger_internal.h"
        macro_note = (r"(?:expanded from|in (?:expansion|definition) of) macro ['‘]" +
                      re.escape(helper) + r"['’]")
        diagnostics = list(re.finditer(
            r"(?m)^([^\n]*?):(\d+):(?:\d+:)?\s*error:\s*([^\n]+)",
            rejected.stderr))
        matching = []
        for index, diagnostic in enumerate(diagnostics):
            path = Path(diagnostic.group(1))
            if path not in (source_path, header_path) or not any(
                    item in diagnostic.group(3) for item in categories):
                continue
            end = (diagnostics[index + 1].start()
                   if index + 1 < len(diagnostics) else len(rejected.stderr))
            block = rejected.stderr[diagnostic.start():end]
            notes = list(re.finditer(
                r"(?m)^([^\n]*?):(\d+):(?:\d+:)?\s*note:\s*([^\n]+)", block))
            helper_notes = [note for note in notes if re.search(macro_note, note.group(3))]
            at_argument_or_call = (path == source_path and
                                   int(diagnostic.group(2)) in (error_line, call_line))
            expanded_at_call = any(Path(note.group(1)) == source_path and
                                   int(note.group(2)) == call_line for note in helper_notes)
            traced_helper = any(Path(note.group(1)) in (source_path, header_path)
                                for note in helper_notes)
            matching.append(bool((at_argument_or_call or expanded_at_call) and
                                 traced_helper and
                                 re.search(r"\b" + re.escape(target_type) + r"\b", block)))
        self.assertTrue(any(matching), rejected.stderr)
        print("ENUM_ATOMIC_TYPE_PAIR_PASS=" + name, flush=True)

    def check_fragment(self, name, negative, positive, category, target_type):
        helper = re.search(r"\b(logger_(?:state|level)_\w+)\(", positive).group(1)
        line = PREAMBLE.count("\n") + 1
        self.check_pair(name, PREAMBLE + negative + "\nreturn 0;\n}\n",
                        PREAMBLE + positive + "\nreturn 0;\n}\n",
                        line, line, helper, category, target_type)

    def function_body(self, source, function):
        # 只定位这些既有 K&R 函数的写点；源码结构漂移时失败，不分析一般 C。
        entries = list(re.finditer(
            r"(?m)^[^\n;{}]*\b" + re.escape(function) + r"\([^;{}]*\)\n\{", source))
        self.assertEqual(1, len(entries), "production function: " + function)
        start = entries[0].end()
        end = re.search(r"(?m)^\}", source[start:])
        self.assertIsNotNone(end, "production function end: " + function)
        return start, source[start:start + end.start()]

    def check_production_value(self, name, filename, function, helper,
                               arguments, value, replacement, target_type):
        source = (ROOT / "src" / filename).read_text(encoding="utf-8")
        start, body = self.function_body(source, function)
        arguments_pattern = r"\s*,\s*".join(re.escape(x.strip()) for x in arguments.split(","))
        pattern = (r"\b" + re.escape(helper) + r"\(\s*" + arguments_pattern +
                   r"\s*,\s*(?P<value>" + re.escape(value) + r")\s*\)")
        sites = list(re.finditer(pattern, body))
        self.assertEqual(1, len(sites), "production mutation site: " + name)
        low, high = sites[0].span("value")
        low += start
        high += start
        negative = source[:low] + replacement + source[high:]
        call_line = source[:start + sites[0].start()].count("\n") + 1
        self.check_pair(name, negative, source, source[:low].count("\n") + 1,
                        call_line, helper, "enum-compare", target_type)

    def test_reject_production_create_expected(self):
        source = (ROOT / "src/logger.c").read_text(encoding="utf-8")
        start, body = self.function_body(source, "create_logger")
        declarations = list(re.finditer(
            r"\blogger_state_t\s+expected\s*=\s*LOGGER_STATE_CREATED\s*;", body))
        self.assertEqual(1, len(declarations), "production expected declaration")
        uses = list(re.finditer(
            r"\blogger_state_try_publish\(\s*&l->state\s*,\s*(?P<expected>&expected)\s*,", body))
        self.assertEqual(1, len(uses), "production expected argument")
        low, high = declarations[0].span()
        low += start
        high += start
        negative = source[:low] + "logger_level_t expected = LOGGER_TRACE;" + source[high:]
        # 声明仍合法且数值仍为零；拒绝必须发生在真实 CAS 的 expected 实参。
        error_line = source[:start + uses[0].start("expected")].count("\n") + 1
        call_line = source[:start + uses[0].start()].count("\n") + 1
        self.check_pair("production_create_expected", negative, source, error_line,
                        call_line, "logger_state_try_publish",
                        "incompatible-pointer-types", "logger_state_t")

    def test_accept_explicit_integer_cast_and_raw_atomic_boundaries(self):
        # 原始整数、显式目标枚举转换和直接 atomic 仍是本契约之外的逃逸。
        # raw 正例仅用整数或显式转换，不假定 GCC/Clang 都接受裸跨枚举参数。
        # 括号函数名可跳过宏；较新编译器仍检查函数枚举参数，此处只传整数。
        body = '''logger_state_init(&state_target, raw);
logger_level_init(&level_target, raw);
(logger_state_init)(&state_target, raw);
logger_state_store_release(&state_target, (logger_state_t)level);
logger_level_store_relaxed(&level_target, (logger_level_t)state);
(void)logger_state_try_publish(&state_target, &expected, raw);
atomic_init(&state_target, raw);
atomic_store_explicit(&state_target, raw, memory_order_release);
(void)atomic_exchange_explicit(&level_target, raw, memory_order_relaxed);
(void)atomic_compare_exchange_strong_explicit(
    &state_target, &expected, (logger_state_t)level,
    memory_order_acq_rel, memory_order_acquire);
'''
        result = self.compile(PREAMBLE + body + "return 0;\n}\n")
        self.assertEqual(0, result.returncode, result.stderr)
        print("ENUM_ATOMIC_TYPE_ESCAPE_PASS", flush=True)


for case in CASES:
    def test(self, name=case):
        self.check_fragment(name, *CASES[name])
    setattr(EnumAtomicTypeContract, "test_reject_" + case, test)

for case in PRODUCTION_CASES:
    def test(self, name=case):
        self.check_production_value(name, *PRODUCTION_CASES[name])
    setattr(EnumAtomicTypeContract, "test_reject_" + case, test)


if __name__ == "__main__":
    unittest.main()
