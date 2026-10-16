#!/usr/bin/env python3
"""核对 fd 迁移边界的限定词法规则，不替代并发证明或运行结果。"""
from __future__ import annotations

from pathlib import Path
import re
import csv
from collections import Counter
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from check_resource_acquisitions import hide_noncode, closing_paren, arguments

RAW = re.compile(r"\b(open|openat|socket|fdopendir|closedir|close)\s*\(")


def violations(source: str) -> list[str]:
    code = hide_noncode(source)
    result = [match.group(1) for match in RAW.finditer(code)]
    if re.search(r"__free\s*\(\s*close_fd\s*\)", code):
        result.append("untracked-cleanup")
    return result


class SnapshotPolicy(unittest.TestCase):
    def test_no_direct_owned_calls_outside_registry(self):
        for path in sorted((ROOT / "src").glob("*.c")):
            if path.name == "logger_fork_fd.c":
                continue
            with self.subTest(path=path.name):
                self.assertEqual(violations(path.read_text()), [])

    def test_logical_acquisition_sites(self):
        with (ROOT / "validation/fork_fd_sites.tsv").open() as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        expected = Counter((r["file"], r["api"], r["arguments"]) for r in rows)
        observed = Counter()
        for path in sorted((ROOT / "src").glob("*.c")):
            if path.name == "logger_fork_fd.c":
                continue
            source = path.read_text()
            code = hide_noncode(source)
            for match in re.finditer(r"\b(logger_fork_(?:open|openat|socket|fdopendir))\s*\(", code):
                opening = code.find("(", match.start(), match.end())
                closing = closing_paren(code, opening)
                self.assertIsNotNone(closing)
                observed[(str(path.relative_to(ROOT)), match.group(1),
                          arguments(source[opening + 1:closing]))] += 1
        self.assertEqual(expected, observed)
        self.assertEqual(sum(expected.values()), 6)

    def test_reject_bypass(self):
        for api in ("open", "openat", "socket", "fdopendir", "close", "closedir"):
            with self.subTest(api=api):
                self.assertEqual(violations(f"int rc = {api}(fd);"), [api])

    def test_reject_wrong_cleanup(self):
        self.assertEqual(violations("int fd __free(close_fd) = -1;"),
                         ["untracked-cleanup"])

    def test_ignore_comments_strings_and_wrappers(self):
        self.assertEqual(violations('/* close(fd); */ const char *s="open(x)";'
                                    ' logger_fork_close(fd);'), [])

    def test_registered_callbacks(self):
        source = (ROOT / "src/logger_process.c").read_text()
        self.assertRegex(source, r"pthread_atfork\(logger_fork_fd_prepare,\s*"
                         r"logger_fork_fd_parent,\s*logger_process_invalidate_child\)")
        self.assertIn("logger_fork_fd_child();", source)

    def test_child_callback_has_no_direct_unsafe_calls(self):
        source = (ROOT / "src/logger_fork_fd.c").read_text()
        start = source.index("void logger_fork_fd_child(void)")
        end = source.index("\n}\n", start) + 3
        child = hide_noncode(source[start:end])
        self.assertNotRegex(child, r"\b(malloc|free|calloc|munmap|flock|"
                            r"pthread_mutex_unlock|pthread_mutex_lock|closedir|"
                            r"pthread_create|printf|vsnprintf)\s*\(")


if __name__ == "__main__":
    unittest.main(verbosity=2)
