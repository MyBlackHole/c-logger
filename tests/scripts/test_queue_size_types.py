#!/usr/bin/env python3
"""用真实队列实现验证条目数/字节数边界；反例均有仅修正类型的正例。"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

# 包含真实实现，以验证三个 static helper，而不是另写相似的声明或算法。
# 片段只做语法/类型检查，不运行占位对象或实际申请队列存储。
PREAMBLE = '''#include "logger_queue.c"
int probe(struct logger_queue_records records, struct logger_queue_bytes bytes,
          size_t raw, struct logger_queue_records *records_output,
          struct logger_queue_bytes *bytes_output)
{
    (void)records;
    (void)bytes;
    (void)raw;
    (void)records_output;
    (void)bytes_output;
'''

CASES = {
    "bytes_as_records": (
        "*records_output = bytes; return 0;",
        "*records_output = records; return 0;",
        ("logger_queue_records", "logger_queue_bytes")),
    "records_as_bytes": (
        "*bytes_output = records; return 0;",
        "*bytes_output = bytes; return 0;",
        ("logger_queue_records", "logger_queue_bytes")),
    "raw_as_records": (
        "*records_output = raw; return 0;",
        "*records_output = records; return 0;",
        ("logger_queue_records",)),
    "raw_as_bytes": (
        "*bytes_output = raw; return 0;",
        "*bytes_output = bytes; return 0;",
        ("logger_queue_bytes",)),
    "bytes_capacity_argument": (
        "return queue_capacity(bytes, &records);",
        "return queue_capacity(records, &records);",
        ("logger_queue_records", "logger_queue_bytes")),
    "raw_capacity_argument": (
        "return queue_capacity(raw, &records);",
        "return queue_capacity(records, &records);",
        ("logger_queue_records",)),
    "bytes_storage_queue_argument": (
        "return queue_storage_size(bytes, records, &bytes);",
        "return queue_storage_size(records, records, &bytes);",
        ("logger_queue_records", "logger_queue_bytes")),
    "bytes_storage_spill_argument": (
        "return queue_storage_size(records, bytes, &bytes);",
        "return queue_storage_size(records, records, &bytes);",
        ("logger_queue_records", "logger_queue_bytes")),
}


class QueueSizeTypeContract(unittest.TestCase):
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
        print("QUEUE_SIZE_TYPE_COMPILER=" + result.stdout.splitlines()[0],
              flush=True)

    def compile(self, source):
        with tempfile.TemporaryDirectory(prefix="logger-queue-size-type-") as work:
            path = Path(work) / "queue_size_case.c"
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
            r"(?m)^[^\n]*[/\\]queue_size_case\.c:(\d+):(?:\d+:)?"
            r"\s*error:\s*([^\n]+)", rejected.stderr)
        matching = [item.group(2) for item in diagnostics
                    if int(item.group(1)) == error_line]
        self.assertTrue(any("incompatible" in item for item in matching),
                        rejected.stderr)
        for expected in types:
            self.assertIn(expected, rejected.stderr)
        print("QUEUE_SIZE_TYPE_PAIR_PASS=" + name, flush=True)

    def check_fragment(self, name, negative, positive, types):
        self.check_pair(name, PREAMBLE + negative + "\n}\n",
                        PREAMBLE + positive + "\n}\n",
                        PREAMBLE.count("\n") + 1, types)

    def check_production_pair(self, name, pattern, replacement):
        source = (ROOT / "src" / "logger_queue.c").read_text(encoding="utf-8")
        sites = list(re.finditer(pattern, source))
        self.assertEqual(1, len(sites), "production mutation site: " + name)
        start, end = sites[0].span("value")
        negative = source[:start] + replacement + source[end:]
        self.check_pair(name, negative, source, source[:start].count("\n") + 1,
                        ("logger_queue_records", "logger_queue_bytes"))

    def test_reject_capacity_output_pointer(self):
        for output, types in (
                ("bytes", ("logger_queue_records", "logger_queue_bytes")),
                ("raw", ("logger_queue_records",))):
            with self.subTest(output=output):
                self.check_fragment("capacity_output_" + output,
                                    "return queue_capacity(records, &" + output + ");",
                                    "return queue_capacity(records, &records);", types)

    def test_reject_production_storage_record_output(self):
        self.check_production_pair(
            "production_storage_record_output",
            r"\bqueue_storage_size\(\s*capacity,\s*spill_capacity,\s*"
            r"(?P<value>&storage_bytes)\s*\)",
            "&capacity")

    def test_reject_production_spill_byte_argument(self):
        # 原数值不变，只把实参的单位改为字节；避免引入未声明或未初始化变量。
        self.check_production_pair(
            "production_spill_byte_argument",
            r"\bqueue_spill_capacity\(\s*(?P<value>capacity)\s*\)",
            "(struct logger_queue_bytes){ capacity.value }")

    def test_accept_same_type_copy_and_explicit_value_escape(self):
        # C 仍允许同型复制、两个 records 参数互换和显式提取/重建裸值。
        # 分配器仍收 size_t；这些可编译边界不代表运行时单位使用正确。
        source = '''struct logger_queue_records record_copy = records;
struct logger_queue_bytes byte_copy = bytes;
records = (struct logger_queue_records){byte_copy.value};
bytes = (struct logger_queue_bytes){record_copy.value};
raw = bytes.value;
void *allocation = logger_runtime_calloc(bytes.value, records.value);
(void)allocation;
(void)raw;
return queue_storage_size(records, record_copy, &byte_copy);
'''
        result = self.compile(PREAMBLE + source + "}\n")
        self.assertEqual(0, result.returncode, result.stderr)
        print("QUEUE_SIZE_TYPE_ESCAPE_PASS", flush=True)


for case in CASES:
    def test(self, name=case):
        negative, positive, types = CASES[name]
        self.check_fragment(name, negative, positive, types)
    setattr(QueueSizeTypeContract, "test_reject_" + case, test)


if __name__ == "__main__":
    unittest.main()
