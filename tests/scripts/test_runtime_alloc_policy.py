#!/usr/bin/env python3
"""运行域分配的逻辑站点清单；这是词法门禁，不是完整数据流证明。"""
import csv
import re
import sys
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from check_resource_acquisitions import hide_noncode, closing_paren, arguments


def allocations(source):
    code = hide_noncode(source)
    result = Counter()
    for match in re.finditer(r"\b(logger_runtime_(?:calloc|malloc))\s*\(", code):
        opening = code.index("(", match.start(), match.end())
        closing = closing_paren(code, opening)
        if closing is None:
            raise ValueError("unclosed allocation")
        result[(match.group(1), arguments(source[opening + 1:closing]))] += 1
    return result


class RuntimeAllocationPolicy(unittest.TestCase):
    def test_sites(self):
        with (ROOT / "validation/runtime_alloc_sites.tsv").open() as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        expected = Counter({(r["file"], r["api"], r["arguments"]): int(r["count"]) for r in rows})
        observed = Counter()
        for path in (ROOT / "src").glob("*.c"):
            if path.name == "logger_runtime.c":
                continue
            for (api, args), count in allocations(path.read_text()).items():
                observed[(str(path.relative_to(ROOT)), api, args)] += count
        self.assertEqual(expected, observed)
        self.assertEqual(sum(expected.values()), 10)

    def test_no_heap_owned_runtime(self):
        for path in (ROOT / "src").glob("*.c"):
            self.assertNotRegex(hide_noncode(path.read_text()), r"\b(?:malloc|calloc|realloc|strdup)\s*\(")

    def test_changed_size_is_not_equal(self):
        self.assertNotEqual(allocations("p=logger_runtime_calloc(n, sizeof(*p));"),
                            allocations("p=logger_runtime_calloc(n+1, sizeof(*p));"))

    def test_comments_are_not_acquisitions(self):
        self.assertEqual(allocations('/* logger_runtime_malloc(n) */ "logger_runtime_calloc(1,2)"'), Counter())

    def test_unclosed_rejected(self):
        with self.assertRaises(ValueError):
            allocations("p=logger_runtime_calloc(n,")


if __name__ == "__main__":
    unittest.main(verbosity=2)
