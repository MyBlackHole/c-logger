#!/usr/bin/env python3
"""用真实 C 编译器验证 fd 类型与返回值检查合同；反例均有对应正例。"""
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
#include "logger_file.h"
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
    "syslog_borrowed_assign": ("struct logger_syslog syslog = {0};\n"
                               "syslog.fd = borrowed;\nreturn 0;",
                               "struct logger_syslog syslog = {0};\n"
                               "syslog.fd = logger_fd_move(owner);\n"
                               "return logger_fd_valid(&syslog.fd);"),
    "syslog_raw_assign": ("struct logger_syslog syslog = {0};\n"
                          "syslog.fd = raw;\nreturn 0;",
                          "struct logger_syslog syslog = {0};\n"
                          "syslog.fd = logger_fd_own(raw);\n"
                          "return logger_fd_valid(&syslog.fd);"),
    "file_borrowed_assign": ("struct logger_file file = LOGGER_FILE_EMPTY;\n"
                             "file.fd = borrowed;\nreturn 0;",
                             "struct logger_file file = LOGGER_FILE_EMPTY;\n"
                             "file.fd = logger_fd_move(owner);\n"
                             "return logger_fd_valid(&file.fd);"),
    "file_raw_assign": ("struct logger_file file = LOGGER_FILE_EMPTY;\n"
                        "file.lock_fd = raw;\nreturn 0;",
                        "struct logger_file file = LOGGER_FILE_EMPTY;\n"
                        "file.lock_fd = logger_fd_move(owner);\n"
                        "return logger_fd_valid(&file.lock_fd);"),
    "file_owned_as_int": ("struct logger_file file = LOGGER_FILE_EMPTY;\n"
                          "file.dir_fd = logger_fd_move(owner);\n"
                          "return fpathconf(file.dir_fd, _PC_NAME_MAX);",
                          "struct logger_file file = LOGGER_FILE_EMPTY;\n"
                          "file.dir_fd = logger_fd_move(owner);\n"
                          "return fpathconf(logger_fd_number("
                          "logger_fd_borrow(&file.dir_fd)), _PC_NAME_MAX);"),
    "syslog_owner_as_int": ("struct logger_syslog syslog = {0};\n"
                            "syslog.fd = logger_fd_own(raw);\n"
                            "return close(syslog.fd);",
                            "struct logger_syslog syslog = {0};\n"
                            "syslog.fd = logger_fd_own(raw);\n"
                            "return close(logger_fd_number("
                            "logger_fd_borrow(&syslog.fd)));"),
}


# 直接包含生产实现，只做语法/类型检查，不链接、不执行这些占位调用。
# 因而真实 static 函数签名一旦退回裸 int，正向对照就会失败。
FILE_PREAMBLE = '''#include "logger_file.c"
int probe(struct logger_file *file, struct logger_owned_fd *owner,
          struct logger_borrowed_fd borrowed, int raw, struct stat *st)
{
    (void)file;
    (void)owner;
    (void)borrowed;
    (void)raw;
    (void)st;
'''
FILE_CASES = {
    "file_candidate_raw_output": (
        "return open_candidate(file, 0, &raw, st);",
        "return open_candidate(file, 0, owner, st);"),
    "file_candidate_borrowed_output": (
        "return open_candidate(file, 0, &borrowed, st);",
        "return open_candidate(file, 0, owner, st);"),
    "file_install_raw_owner": (
        "install_fd(file, raw, st, 0);\nreturn 0;",
        "install_fd(file, owner, st, 0);\nreturn 0;"),
    "file_install_borrowed_owner": (
        "install_fd(file, &borrowed, st, 0);\nreturn 0;",
        "install_fd(file, owner, st, 0);\nreturn 0;"),
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

    def compile(self, source, preamble=PREAMBLE, object_code=False):
        with tempfile.TemporaryDirectory(prefix="logger-fd-type-") as work:
            path = Path(work) / "fd_type_case.c"
            path.write_text(preamble + source + "\n}\n", encoding="utf-8")
            command = self.compiler + [
                "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "include"), str(path),
            ]
            # GCC 的 unused-result 诊断需要实际对象编译；不链接或执行片段。
            command += (["-c", "-o", str(Path(work) / "fd_type_case.o")]
                        if object_code else ["-fsyntax-only"])
            return subprocess.run(command, capture_output=True, text=True,
                                  timeout=20, env=self.env)

    def check_pair(self, name, cases=CASES, preamble=PREAMBLE):
        negative, positive = cases[name]
        accepted = self.compile(positive, preamble)
        self.assertEqual(0, accepted.returncode, accepted.stderr)
        rejected = self.compile(negative, preamble)
        self.assertGreater(rejected.returncode, 0, rejected.stderr)
        # 错误必须落在测试片段，并具有类型诊断，不能把任意构建失败算作拒绝。
        self.assertIn("fd_type_case.c:", rejected.stderr)
        self.assertRegex(rejected.stderr,
                         r"incompatible|invalid initializer|invalid initialization|int-conversion")
        print("FD_TYPE_PAIR_PASS=" + name, flush=True)

    def test_required_results_remain_checked(self):
        preamble = PREAMBLE.replace('#include "logger_fd.h"',
                                    '#include "logger_cleanup.h"\n#include "logger_fd.h"')
        disabled = preamble.replace('#include "logger_cleanup.h"',
                                    '#include "logger_compiler.h"\n'
                                    '#undef LOGGER_MUST_CHECK\n#define LOGGER_MUST_CHECK\n'
                                    '#include "logger_cleanup.h"')
        cases = (
            ("logger_fd_own", "logger_fd_own(raw)",
             "struct logger_owned_fd result = logger_fd_own(raw);\n"
             "return logger_fd_valid(&result);"),
            ("logger_fd_take", "logger_fd_take(owner)", "return logger_fd_take(owner);"),
            ("logger_fd_move", "logger_fd_move(owner)",
             "struct logger_owned_fd result = logger_fd_move(owner);\n"
             "return logger_fd_valid(&result);"),
            ("logger_fd_close_status", "logger_fd_close_status(owner)",
             "return logger_fd_close_status(owner);"),
            ("logger_fd_fdopendir", "logger_fd_fdopendir(owner)",
             "return logger_fd_fdopendir(owner) != NULL;"),
            ("__cleanup_must_check_ptr", "__cleanup_must_check_ptr(owner)",
             "return __cleanup_must_check_ptr(owner) != NULL;"),
        )
        for name, call, used in cases:
            with self.subTest(function=name):
                accepted = self.compile(used, preamble, object_code=True)
                self.assertEqual(0, accepted.returncode, accepted.stderr)
                ignored = call + ";\nreturn 0;"
                rejected = self.compile(ignored, preamble, object_code=True)
                self.assertGreater(rejected.returncode, 0, rejected.stderr)
                self.assertIn("unused-result", rejected.stderr)
                self.assertIn(name, rejected.stderr)
                # 只撤掉实际头文件注解后，同一片段必须可编译，排除无关错误。
                control = self.compile(ignored, disabled, object_code=True)
                self.assertEqual(0, control.returncode, control.stderr)

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

for case in FILE_CASES:
    def test(self, name=case):
        self.check_pair(name, FILE_CASES, FILE_PREAMBLE)
    setattr(FdTypeContract, "test_reject_" + case, test)

if __name__ == "__main__":
    unittest.main()
