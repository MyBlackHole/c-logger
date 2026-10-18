#!/usr/bin/env python3
"""真实 Clang 正反例；编译失败与命中所有权规则严格分开。"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import check_owned_fd_copies as checker

PREAMBLE = '''#define _GNU_SOURCE
#include "logger_internal.h"
typedef struct logger_owned_fd owner_alias;
typedef owner_alias owner_alias2;
struct embedded { owner_alias2 slots[2]; };
struct outer { struct embedded inner; };
struct borrowed_wrapper { struct logger_owned_fd *pointer; };
void sink(struct logger_owned_fd);
#define COPY(dst, src) ((dst) = (src))
int probe(struct logger_owned_fd *dst, struct logger_owned_fd *src,
          logger_file_t *file, logger_file_t *other,
          logger_syslog_t *syslog, logger_syslog_t *other_syslog,
          struct outer *container, struct outer *other_container,
          int condition)
{
    (void)dst; (void)src; (void)file; (void)other;
    (void)syslog; (void)other_syslog;
    (void)container; (void)other_container; (void)condition;
'''

# 每个反例都能被 C 编译器接受，只因 AST 所有权规则而拒绝。
CASES = {
    "assignment": ("*dst = *src;", "*dst = logger_fd_move(src);"),
    "initializer": ("struct logger_owned_fd copy = *src; (void)copy;",
                    "struct logger_owned_fd copy = logger_fd_move(src); (void)copy;"),
    "typedef_chain": ("owner_alias2 copy = *src; (void)copy;",
                      "owner_alias2 copy = logger_fd_move(src); (void)copy;"),
    "chained_assignment": ("*dst = file->fd = logger_fd_move(src);",
                           "file->fd = logger_fd_move(src); *dst = logger_fd_move(&file->fd);"),
    "discarded_assignment": ("(void)(*dst = *src);", "(void)(*dst = logger_fd_move(src));"),
    "if_assignment": ("if (condition) *dst = *src;", "if (condition) *dst = logger_fd_move(src);"),
    "macro": ("COPY(*dst, *src);", "COPY(*dst, logger_fd_move(src));"),
    "argument": ("sink(*src);", "sink(logger_fd_move(src));"),
    "indirect_argument": ("void (*send_owner)(struct logger_owned_fd) = sink; send_owner(*src);",
                          "void (*send_owner)(struct logger_owned_fd) = sink; send_owner(logger_fd_move(src));"),
    "file_container": ("*file = *other;", "*file = LOGGER_FILE_EMPTY;"),
    "syslog_container": ("*syslog = *other_syslog;",
                         "*syslog = (logger_syslog_t){.fd = {-1}};"),
    "nested_array": ("*container = *other_container;",
                     "*container = (struct outer){.inner = {.slots = {{-1}, {-1}}}};"),
    "const_source": ("const struct logger_owned_fd *view = src; *dst = *view;",
                     "const struct logger_owned_fd *view = src; (void)logger_fd_valid(view);"),
    "conditional": ("*dst = condition ? *src : logger_fd_own(-1);",
                    "*dst = condition ? logger_fd_move(src) : logger_fd_own(-1);"),
    "compound_member": ("*syslog = (logger_syslog_t){.fd = *src};",
                        "*syslog = (logger_syslog_t){.fd = logger_fd_move(src)};"),
    "statement_expression": ("*dst = ({ *src; });",
                             "*dst = ({ logger_fd_move(src); });"),
    "generic_selected": ("*dst = _Generic(condition, int: *src);",
                         "*dst = _Generic(condition, int: logger_fd_move(src));"),
}


class OwnedCopies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = os.environ.get("LOGGER_TEST_CLANG", "clang")
        cls.flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                     "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src")]
        version = subprocess.run([cls.compiler, "--version"], check=True,
                                 capture_output=True, text=True, timeout=10)
        print("OWNED_COPY_COMPILER=" + version.stdout.splitlines()[0], flush=True)

    def analyze(self, body, full=False):
        with tempfile.TemporaryDirectory(prefix="logger-owned-copy-") as work:
            source = Path(work) / "probe.c"
            source.write_text(body if full else PREAMBLE + body + "\nreturn 0;\n}\n")
            return checker.analyze(self.compiler, self.flags, str(source), str(ROOT))

    def pair(self, negative, positive):
        accepted = self.analyze(positive)
        self.assertTrue(accepted["owner_type_seen"])
        self.assertEqual([], accepted["violations"])
        rejected = self.analyze(negative)
        self.assertTrue(rejected["violations"])
        self.assertTrue(all(v["rule"] == "owned_fd_value_copy" and
                            v["function"] == "probe" and v["range"]
                            for v in rejected["violations"]))

    def test_real_file_handoff_mutants(self):
        source = (ROOT / "src/logger_file.c").read_text()
        self.assertEqual([], self.analyze(source, full=True)["violations"])
        for before, after, function in [
            ("*out = logger_fd_move(&fd);", "*out = fd;", "open_candidate"),
            ("f->fd = logger_fd_move(fd);", "f->fd = *fd;", "install_fd"),
        ]:
            with self.subTest(function=function):
                self.assertEqual(1, source.count(before))
                report = self.analyze(source.replace(before, after), full=True)
                self.assertTrue(any(v["function"] == function for v in report["violations"]))

    def test_return_copy(self):
        source = '#include "logger_fd.h"\nstruct logger_owned_fd probe(struct logger_owned_fd *p) { return %s; }'
        self.assertTrue(self.analyze(source % "*p", full=True)["violations"])
        self.assertEqual([], self.analyze(source % "logger_fd_move(p)", full=True)["violations"])

    def test_pointer_and_borrowed_copy_are_not_ownership(self):
        self.assertEqual([], self.analyze('''
            struct borrowed_wrapper a = {src}, b = a; (void)b;
            struct logger_borrowed_fd view = logger_fd_borrow(src), next = view;
            (void)logger_fd_number(next);
            (void)sizeof(*src);
            (void)_Generic(*src, struct logger_owned_fd: 1);
        ''')["violations"])

    def test_raw_memory_and_integer_escape_are_explicit_limits(self):
        self.assertEqual([], self.analyze('''
            memcpy(dst, src, sizeof(*src));
            dst->value = src->value;
        ''')["violations"])

    def test_preprocessor_configuration_is_not_all_configurations(self):
        source = PREAMBLE + '#if LOGGER_ENABLE_LOCKDEP\n*dst = *src;\n#endif\nreturn 0;}\n'
        old = self.flags
        try:
            self.flags = old + ["-DLOGGER_ENABLE_LOCKDEP=0"]
            self.assertEqual([], self.analyze(source, full=True)["violations"])
            self.flags = old + ["-DLOGGER_ENABLE_LOCKDEP=1"]
            self.assertTrue(self.analyze(source, full=True)["violations"])
        finally:
            self.flags = old

    def test_syntax_error_is_not_a_copy_detection(self):
        with self.assertRaisesRegex(ValueError, "clang_diagnostic"):
            self.analyze("invalid code here;")

    def test_missing_compiler_fails(self):
        with self.assertRaises(OSError):
            checker.analyze("/nonexistent/logger-test-clang", [], "none.c", str(ROOT))

    def test_timeout_fails(self):
        with mock.patch.object(checker.subprocess, "run", side_effect=subprocess.TimeoutExpired("clang", 30)):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.analyze("*dst = logger_fd_move(src);")

    def test_nonzero_and_stderr_are_not_accepted(self):
        for code, stderr in [(77, b""), (-11, b""), (0, b"warning: test")]:
            with self.subTest(code=code, stderr=stderr):
                with mock.patch.object(checker.subprocess, "run", return_value=subprocess.CompletedProcess([], code, b"{}", stderr)):
                    with self.assertRaisesRegex(ValueError, "clang_diagnostic"):
                        self.analyze("return 0;")

    def test_invalid_ast_fails(self):
        for ast in [None, {}, [], {"kind": "TranslationUnitDecl", "inner": []}]:
            with self.subTest(ast=ast), self.assertRaises(ValueError):
                checker.inspect_ast(ast)

    def test_invalid_json_fails(self):
        with mock.patch.object(checker.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"not-json", b"")):
            with self.assertRaises(ValueError):
                self.analyze("return 0;")

    def test_empty_manifest_fails(self):
        with self.assertRaises(ValueError):
            checker.run_manifest({"schema": 1, "units": []})


for name, pair in CASES.items():
    def test(self, values=pair):
        self.pair(*values)
    setattr(OwnedCopies, "test_reject_" + name, test)

if __name__ == "__main__":
    unittest.main(verbosity=2)
