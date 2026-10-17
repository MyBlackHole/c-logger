#!/usr/bin/env python3
"""用真实 C 编译器验证 fd 类型合同；每个反例均有只修正类型的正例。"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PREAMBLE = '''#define _GNU_SOURCE
#include "logger_fd.h"
#include "logger_syslog.h"
int probe(struct logger_owned_fd *owner, struct logger_borrowed_fd borrowed,
          int raw)
{
    (void)owner;
    (void)borrowed;
    (void)raw;
'''

# 只进行编译，不运行这些含借用/所有者占位对象的片段。
CASES = {
    "borrowed_close": ("return logger_fd_close_status(&borrowed);",
                       "return logger_fd_close_status(owner);"),
    "borrowed_take": ("return logger_fd_take(&borrowed);",
                      "return logger_fd_take(owner);"),
    "borrowed_directory": ("return logger_fd_fdopendir(&borrowed) != NULL;",
                           "return logger_fd_fdopendir(owner) != NULL;"),
    "raw_close": ("return logger_fd_close_status(&raw);",
                  "return logger_fd_close_status(owner);"),
    "raw_take": ("return logger_fd_take(&raw);",
                 "return logger_fd_take(owner);"),
    "raw_directory": ("return logger_fd_fdopendir(&raw) != NULL;",
                      "return logger_fd_fdopendir(owner) != NULL;"),
    "owned_as_number": ("return logger_fd_number(*owner);",
                        "return logger_fd_number(borrowed);"),
    "borrowed_as_owner": ("struct logger_owned_fd copy = borrowed;\n"
                          "return logger_fd_take(&copy);",
                          "struct logger_owned_fd copy = logger_fd_own(raw);\n"
                          "return logger_fd_take(&copy);"),
    "integer_as_owner": ("struct logger_owned_fd copy = raw;\n"
                         "return logger_fd_take(&copy);",
                         "struct logger_owned_fd copy = logger_fd_own(raw);\n"
                         "return logger_fd_take(&copy);"),
    "borrowed_cleanup": ("logger_fd_cleanup(&borrowed);\nreturn 0;",
                         "logger_fd_cleanup(owner);\nreturn 0;"),
    "borrowed_move": ("struct logger_owned_fd newfd = logger_fd_move(&borrowed);\n"
                      "return logger_fd_valid(&newfd);",
                      "struct logger_owned_fd newfd = logger_fd_move(owner);\n"
                      "return logger_fd_valid(&newfd);"),
    "syslog_borrowed_assign": ("logger_syslog_t syslog = {0};\n"
                               "syslog.fd = borrowed;\nreturn 0;",
                               "logger_syslog_t syslog = {0};\n"
                               "syslog.fd = logger_fd_move(owner);\n"
                               "return logger_fd_valid(&syslog.fd);"),
    "syslog_raw_assign": ("logger_syslog_t syslog = {0};\n"
                          "syslog.fd = raw;\nreturn 0;",
                          "logger_syslog_t syslog = {0};\n"
                          "syslog.fd = logger_fd_own(raw);\n"
                          "return logger_fd_valid(&syslog.fd);"),
    "syslog_owner_as_int": ("logger_syslog_t syslog = {0};\n"
                            "syslog.fd = logger_fd_own(raw);\n"
                            "return close(syslog.fd);",
                            "logger_syslog_t syslog = {0};\n"
                            "syslog.fd = logger_fd_own(raw);\n"
                            "return close(logger_fd_number("
                            "logger_fd_borrow(&syslog.fd)));"),
}


class FdTypeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shlex.split(os.environ.get("LOGGER_TEST_CC", "cc"))
        if not cls.compiler:
            raise ValueError("LOGGER_TEST_CC")
        cls.env = dict(os.environ, LC_ALL="C")
        # 缺少编译器/头文件/不兼容选项必须失败，不能算作反例拒绝成功。
        result = subprocess.run(cls.compiler + ["--version"],
                                capture_output=True, text=True,
                                timeout=20, env=cls.env, check=True)
        print("FD_TYPE_COMPILER=" + result.stdout.splitlines()[0], flush=True)

    def compile(self, source):
        with tempfile.TemporaryDirectory(prefix="logger-fd-type-") as work:
            path = Path(work) / "fd_type_case.c"
            path.write_text(PREAMBLE + source + "\n}\n", encoding="utf-8")
            command = self.compiler + [
                "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-fsyntax-only", "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "include"), str(path),
            ]
            return subprocess.run(command, capture_output=True, text=True,
                                  timeout=20, env=self.env)

    def check_pair(self, name):
        negative, positive = CASES[name]
        accepted = self.compile(positive)
        self.assertEqual(0, accepted.returncode, accepted.stderr)
        rejected = self.compile(negative)
        self.assertGreater(rejected.returncode, 0, rejected.stderr)
        # 错误必须落在测试片段，并具有类型诊断，不能把任意构建失败算作拒绝。
        self.assertIn("fd_type_case.c:", rejected.stderr)
        self.assertRegex(rejected.stderr,
                         r"incompatible|invalid initializer|invalid initialization")
        print("FD_TYPE_PAIR_PASS=" + name, flush=True)

    def test_explicit_escape_is_outside_type_guarantee(self):
        # C 允许同型复制和显式裸值逃逸。正向编译记录此限制，不运行危险副本。
        result = self.compile("struct logger_owned_fd copy = *owner;\n"
                              "(void)copy;\n"
                              "return close(logger_fd_number(borrowed));")
        self.assertEqual(0, result.returncode, result.stderr)


for case in CASES:
    def test(self, name=case):
        self.check_pair(name)
    setattr(FdTypeContract, "test_reject_" + case, test)

if __name__ == "__main__":
    unittest.main()
