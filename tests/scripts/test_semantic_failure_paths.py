#!/usr/bin/env python3
"""#106 资源与同步语义门禁的真实正反例，不修改生产代码。"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "semantic_failure_paths", ROOT / "scripts" / "check_semantic_failure_paths.py"
)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class SemanticFailurePaths(unittest.TestCase):
    def fixture(self, source: str, filename: str = "src/probe.c") -> list[str]:
        with tempfile.TemporaryDirectory(prefix="c-logger-semantic-") as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "include").mkdir()
            path = root / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            return GATE.check_tree(root)

    def test_current_production_source_has_no_violations(self):
        self.assertEqual([], GATE.check_tree(ROOT))

    def test_reject_all_discarded_rwlock_and_condition_results(self):
        violations = (
            "pthread_rwlock_rdlock(&lock);",
            "pthread_rwlock_wrlock(&lock);",
            "pthread_rwlock_tryrdlock(&lock);",
            "pthread_rwlock_trywrlock(&lock);",
            "(void)pthread_rwlock_unlock(&lock);",
            "pthread_cond_wait(&cond, &lock);",
            "pthread_cond_timedwait(&cond, &lock, &ts);",
            "pthread_cond_clockwait(&cond, &lock, CLOCK_MONOTONIC, &ts);",
            "__cleanup_lockdep_rwlock_wrlock(&lock);",
            "if (enabled) pthread_rwlock_unlock(&lock);",
        )
        for statement in violations:
            with self.subTest(statement=statement):
                errors = self.fixture("void probe(void) {\n" + statement + "\n}\n")
                self.assertEqual(1, len(errors), errors)
                self.assertIn("返回值被丢弃", errors[0])

    def test_checked_results_are_accepted(self):
        valid = (
            "int rc = pthread_rwlock_wrlock(&rw);\n"
            "if (rc) return rc;\n"
            "rc = pthread_cond_wait(&cv, &mu);\n"
            "if (rc) return rc;\n"
            "return pthread_rwlock_unlock(&rw);\n"
        )
        self.assertEqual([], self.fixture("int probe(void) {\n" + valid + "}"))

    def test_multiline_return_value_use_is_accepted(self):
        # 真实 worker 采用这种跨行赋值；不得把右值单独判为裸调用。
        source = (
            "int f(void) {\n"
            "int rc =\n"
            "    pthread_cond_wait(&cv, &mu);\n"
            "if (rc) return rc;\n"
            "return\n"
            "    pthread_rwlock_unlock(&rw);\n"
            "}\n"
        )
        self.assertEqual([], self.fixture(source))

    def test_close_eintr_retry_do_while_is_rejected(self):
        source = (
            "void probe(void) {\n"
            "int rc;\n"
            "do { rc = close(fd); } while (rc < 0 && errno == EINTR);\n"
            "}\n"
        )
        errors = self.fixture(source)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("禁止 close()", errors[0])

    def test_close_eintr_retry_while_condition_is_rejected(self):
        errors = self.fixture(
            "void f(void) { while (close(fd) < 0 && errno == EINTR) {} }"
        )
        self.assertEqual(1, len(errors), errors)
        self.assertIn("禁止 close()", errors[0])

    def test_close_eintr_retry_while_body_is_rejected(self):
        errors = self.fixture(
            "void f(void) { while (errno == EINTR) { close(fd); } }"
        )
        self.assertEqual(1, len(errors), errors)
        self.assertIn("禁止 close()", errors[0])

    def test_valid_fsync_retry_followed_by_single_close(self):
        source = (
            "int f(int fd) {\n"
            "int rc;\n"
            "do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);\n"
            "if (close(fd) < 0) return -errno;\n"
            "return rc;\n"
            "}\n"
        )
        self.assertEqual([], self.fixture(source))

    def test_comments_and_strings_do_not_trigger(self):
        text = (
            "// pthread_cond_wait(&cond, &mu);\n"
            "/* do { close(fd); } while (errno == EINTR); */\n"
            "const char *s = \"while (close(fd) && errno == EINTR)\";\n"
            "const char ch = ';';\n"
        )
        self.assertEqual([], self.fixture(text, "include/a.h"))

    def test_line_number_is_reported(self):
        failures = self.fixture(
            "\n\n\nvoid f(void) {\n"
            "pthread_cond_wait(&cv, &mu);\n"
            "}\n"
        )
        self.assertEqual(1, len(failures))
        self.assertIn("src/probe.c:5:", failures[0])

    def test_missing_source_tree_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = GATE.check_tree(Path(tmp))
        self.assertEqual(["src: 生产源码目录不存在"], result)


if __name__ == "__main__":
    unittest.main()
