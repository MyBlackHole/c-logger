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
#define COPY_BYTES(dst, src) memcpy(((void *)(dst)), ((const void *)(src)), sizeof(*(src)))
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

    def memory_copy(self, body, callee="memcpy", indices=(0, 1),
                    type_names=None, full=False, function="probe"):
        report = self.analyze(body, full=full)
        self.assertTrue(report["owner_type_seen"])
        self.assertEqual(1, len(report["violations"]), report["violations"])
        violation = report["violations"][0]
        self.assertEqual("owned_fd_memory_copy", violation["rule"])
        self.assertEqual(function, violation["function"])
        self.assertEqual(callee, violation["callee"])
        self.assertTrue(violation["range"].get("begin"))
        self.assertTrue(violation["range"].get("end"))
        self.assertEqual(list(indices), [argument["index"]
                                       for argument in violation["arguments"]])
        for position, argument in enumerate(violation["arguments"]):
            with self.subTest(argument=argument["index"]):
                info = argument["type"]
                name = info.get("desugaredQualType", info.get("qualType", ""))
                self.assertIn("*", name)
                self.assertTrue(argument["range"].get("begin"))
                self.assertTrue(argument["range"].get("end"))
                if type_names:
                    self.assertIn(type_names[position], name)
        return violation

    def test_memory_library_and_builtins(self):
        for callee in ("memcpy", "memmove", "__builtin_memcpy", "__builtin_memmove",
                       "__builtin___memcpy_chk", "__builtin___memmove_chk"):
            with self.subTest(callee=callee):
                bound = ", sizeof(*dst)" if callee.endswith("_chk") else ""
                self.memory_copy(f"{callee}(dst, src, sizeof(*dst){bound});",
                                 callee=callee,
                                 type_names=("logger_owned_fd", "logger_owned_fd"))

    def test_memory_typedef_and_embedded_arrays(self):
        for body, name in [
            ('''typedef owner_alias2 *owner_pointer;
                owner_pointer a = dst, b = src;
                memcpy(a, b, sizeof(*a));''', "owner_alias2"),
            ('''memcpy(container->inner.slots, other_container->inner.slots,
                       sizeof(container->inner.slots));''', "owner_alias2"),
            ("memcpy(container, other_container, sizeof(*container));", "struct outer"),
            ("memcpy(file, other, sizeof(*file));", "logger_file"),
            ("memcpy(syslog, other_syslog, sizeof(*syslog));", "logger_syslog"),
        ]:
            with self.subTest(body=body):
                self.memory_copy(body, type_names=(name, name))

    def test_memory_pointer_to_array(self):
        self.memory_copy('''
            owner_alias2 (*a)[2] = &container->inner.slots;
            owner_alias2 (*b)[2] = &other_container->inner.slots;
            memcpy(a, b, sizeof(*a));
        ''', type_names=("owner_alias2", "owner_alias2"))
        self.memory_copy('''
            memcpy(&container->inner.slots, &other_container->inner.slots,
                   sizeof(container->inner.slots));
        ''', type_names=("owner_alias2", "owner_alias2"))

    def test_memory_anonymous_embedded_owners(self):
        for declaration in [
            "union { struct logger_owned_fd fd; int n; };",
            "struct { struct logger_owned_fd fd; } member;",
        ]:
            with self.subTest(declaration=declaration):
                source = '#include <string.h>\n#include "logger_fd.h"\n'
                source += 'struct wrap { %s };\n' % declaration
                source += 'void probe(struct wrap *d, struct wrap *s) { memcpy(d, s, sizeof(*d)); }'
                self.memory_copy(source, full=True, type_names=("struct wrap", "struct wrap"))

    def test_memory_only_one_owner_argument(self):
        for body, index in [
            ("memcpy(dst, bytes, sizeof(*dst));", 0),
            ("memcpy(bytes, src, sizeof(*src));", 1),
        ]:
            with self.subTest(index=index):
                self.memory_copy("unsigned char bytes[sizeof(*dst)] = {0};\n" + body,
                                 indices=(index,), type_names=("logger_owned_fd",))

    def test_memory_cast_parenthesis_and_macro(self):
        for body in [
            "(memcpy)(((dst)), ((src)), sizeof(*dst));",
            "(&memcpy)(dst, src, sizeof(*dst));",
            "(*&memcpy)(dst, src, sizeof(*dst));",
            "memcpy((void *)(unsigned char *)dst, (const void *)(const char *)src, sizeof(*dst));",
            "COPY_BYTES(dst, src);",
            "memcpy(&file->fd, &other->fd, sizeof(file->fd));",
            "const struct logger_owned_fd *view = src; memcpy(dst, view, sizeof(*dst));",
            "memcpy(dst + 1, src + 1, sizeof(*dst));",
        ]:
            with self.subTest(body=body):
                self.memory_copy(body, type_names=("logger_owned_fd", "logger_owned_fd"))

    def test_memory_discarded_and_selected_calls(self):
        for body in [
            "(void)memcpy(dst, src, sizeof(*dst));",
            "if (condition) memcpy(dst, src, sizeof(*dst));",
            "if (memcpy(dst, src, sizeof(*dst))) condition = 0;",
            "(void)({ memcpy(dst, src, sizeof(*dst)); 0; });",
            "(void)_Generic(condition, int: memcpy(dst, src, sizeof(*dst)));",
        ]:
            with self.subTest(body=body):
                self.memory_copy(body)

    def test_memory_length_and_same_object(self):
        # 有限门禁不根据长度或地址相等推导所有权转移是否安全。
        for body in [
            "memcpy(dst, src, 0);",
            "memcpy(dst, src, 1);",
            "memcpy(dst, dst, sizeof(*dst));",
        ]:
            with self.subTest(body=body):
                self.memory_copy(body)

    def test_memory_non_owner_members(self):
        self.assertEqual([], self.analyze('''
            memcpy(file->path, other->path, sizeof(file->path));
            memmove(file->name, other->name, sizeof(file->name));
            __builtin_memcpy(syslog->ident, other_syslog->ident, sizeof(syslog->ident));
            memcpy(&file->path, &other->path, sizeof(file->path));
            memcpy((void *)file->name, (const void *)other->name, sizeof(file->name));
            memcpy(&dst->value, &src->value, sizeof(dst->value));
        ''')["violations"])

    def test_memory_pointer_storage_and_borrowed_containers(self):
        self.assertEqual([], self.analyze('''
            struct logger_owned_fd *a = dst, *b = src;
            struct logger_owned_fd *pointers[2] = {dst, src};
            struct logger_owned_fd *next[2] = {dst, src};
            struct borrowed_wrapper first = {dst}, second = {src};
            struct logger_borrowed_fd view = logger_fd_borrow(src);
            struct logger_borrowed_fd copy = logger_fd_borrow(dst);
            struct { struct logger_owned_fd fd; } *borrow = 0, *other_borrow = 0;
            memcpy(&a, &b, sizeof(a));
            memcpy(pointers, next, sizeof(pointers));
            memcpy(&pointers, &next, sizeof(pointers));
            memcpy(&first, &second, sizeof(first));
            memcpy(&copy, &view, sizeof(copy));
            memcpy(&borrow, &other_borrow, sizeof(borrow));
        ''')["violations"])

    def test_memory_unevaluated_calls(self):
        self.assertEqual([], self.analyze('''
            (void)sizeof(memcpy(dst, src, sizeof(*dst)));
            (void)_Generic(memcpy(dst, src, sizeof(*dst)), void *: 1);
            (void)_Generic(condition, int: (void *)0,
                           default: memcpy(dst, src, sizeof(*dst)));
        ''')["violations"])

    def test_anonymous_embedded_owners_and_pointer_boundary(self):
        for declaration in [
            "union { struct logger_owned_fd fd; int n; };",
            "struct { struct logger_owned_fd fd; } member;",
        ]:
            source = '#include "logger_fd.h"\nstruct wrap { %s };\n' % declaration
            source += 'void probe(struct wrap *d, struct wrap *s) { *d = *s; }'
            self.assertTrue(self.analyze(source, full=True)["violations"])
        source = '#include "logger_fd.h"\n'
        source += 'struct wrap { struct { struct logger_owned_fd fd; } *borrow; };\n'
        source += 'void probe(struct wrap *d, struct wrap *s) { *d = *s; }'
        self.assertEqual([], self.analyze(source, full=True)["violations"])

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

    def test_real_file_handoff_memory_mutants(self):
        source = (ROOT / "src/logger_file.c").read_text()
        self.assertEqual([], self.analyze(source, full=True)["violations"])
        for callee in ("memcpy", "memmove"):
            for before, after, function in [
                ("*out = logger_fd_move(&fd);", f"{callee}(out, &fd, sizeof(fd));", "open_candidate"),
                ("f->fd = logger_fd_move(fd);", f"{callee}(&f->fd, fd, sizeof(*fd));", "install_fd"),
            ]:
                with self.subTest(callee=callee, function=function):
                    self.assertEqual(1, source.count(before))
                    self.memory_copy(source.replace(before, after), callee=callee,
                                     type_names=("logger_owned_fd", "logger_owned_fd"),
                                     full=True, function=function)

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

    def test_indirect_memory_and_integer_escape_are_explicit_limits(self):
        self.assertEqual([], self.analyze('''
            void *destination = dst;
            const void *source = src;
            void *(*copy_bytes)(void *, const void *, size_t) = memcpy;
            void *custom_copy(void *, const void *, size_t);
            memcpy(destination, source, sizeof(*src));
            copy_bytes(dst, src, sizeof(*src));
            custom_copy(dst, src, sizeof(*src));
            memcpy((char *)dst + 1, (const char *)src + 1, sizeof(*dst) - 1);
            memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, sizeof(*dst));
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
