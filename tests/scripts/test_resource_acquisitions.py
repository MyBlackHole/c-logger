#!/usr/bin/env python3
"""#106 生产资源获取清单的正反例与源码变异门禁。"""
from __future__ import annotations

import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "resource_acquisitions", ROOT / "scripts" / "check_resource_acquisitions.py"
)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)

SAMPLE = "void create(void) { void *p = calloc(1, sizeof(*p)); }\n"
ROW = {
    "file": "src/sample.c",
    "api": "calloc",
    "arguments": "1,sizeof(*p)",
    "count": "1",
    "owner": "候选实例独占分配",
    "failure_test": "tests/regression/sample.c",
    "coverage": "direct",
    "rationale": "创建失败后所有权不发布，由宿主确认对象销毁",
}


class ResourceAcquisitions(unittest.TestCase):
    def fixture(self, source=SAMPLE, entries=None):
        temp = tempfile.TemporaryDirectory(prefix="c-logger-acquisition-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        for dirpath in ("src", "include", "tests/regression", "validation"):
            (root / dirpath).mkdir(parents=True)
        (root / "src/sample.c").write_text(source, encoding="utf-8")
        (root / "tests/regression/sample.c").write_text(
            "int main(void) { return 0; }\n", encoding="utf-8"
        )
        self.write_manifest(root, [ROW.copy()] if entries is None else entries)
        return root

    @staticmethod
    def write_manifest(root, rows, fields=GATE.FIELDS):
        with (root / GATE.MANIFEST).open(
            "w", encoding="utf-8", newline=""
        ) as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)

    def test_current_production_acquisitions_reviewed(self):
        count, errors = GATE.check(ROOT)
        self.assertEqual([], errors)
        self.assertEqual(22, count)

    def test_real_resource_call_matches_inventory(self):
        root = self.fixture()
        self.assertEqual((1, []), GATE.check(root))

    def test_multiline_argument_spacing_does_not_change_identity(self):
        root = self.fixture(
            "void create(void) {\n"
            " void *p = calloc(\n"
            "     1,\n"
            "     sizeof(*p));\n"
            "}\n"
        )
        self.assertEqual((1, []), GATE.check(root))

    def test_comments_strings_and_characters_are_not_calls(self):
        source = (
            "/* calloc(1, sizeof(*p)); */\n"
            "// socket(AF_UNIX, 0, 0);\n"
            'const char *s = "openat(1, 2, 3)";\n'
            "char c = '(';\n"
        )
        root = self.fixture(source, [])
        observed, errors = GATE.scan(root)
        self.assertEqual([], errors)
        self.assertEqual(0, sum(observed.values()))
        with self.assertRaisesRegex(ValueError, "为空"):
            GATE.inventory(root)

    def test_new_acquisition_is_rejected_until_reviewed(self):
        root = self.fixture(
            SAMPLE + "void extra(void) { int fd = open(path, O_RDONLY); }\n"
        )
        count, errors = GATE.check(root)
        self.assertEqual(2, count)
        self.assertTrue(any("未评审" in item and "open" in item
                            for item in errors), errors)

    def test_removed_acquisition_is_reported(self):
        root = self.fixture("void create(void) {}\n")
        count, errors = GATE.check(root)
        self.assertEqual(0, count)
        self.assertTrue(any("已缺失" in item for item in errors), errors)

    def test_changed_allocation_size_is_new_ownership_review(self):
        root = self.fixture(
            "void create(void) { void *p = calloc(2, sizeof(*p)); }\n"
        )
        count, errors = GATE.check(root)
        self.assertEqual(1, count)
        self.assertEqual(2, len(errors))

    def test_literal_path_changes_openat_call_identity(self):
        source = 'int fd = openat(dirfd, ".", O_RDONLY);\n'
        root = self.fixture(source, [])
        observed, errors = GATE.scan(root)
        self.assertEqual([], errors)
        self.assertEqual(
            {("src/sample.c", "openat", 'dirfd,".",O_RDONLY'): 1},
            dict(observed),
        )
        modified, errors = GATE.scan(self.modified(root, source.replace('"."', '"next"')))
        self.assertEqual([], errors)
        self.assertNotEqual(observed, modified)

    @staticmethod
    def modified(root, source):
        (root / "src/sample.c").write_text(source, encoding="utf-8")
        return root

    def test_changed_repeated_call_count_is_rejected(self):
        root = self.fixture(SAMPLE + SAMPLE)
        count, errors = GATE.check(root)
        self.assertEqual(2, count)
        self.assertEqual(1, len(errors))

    def test_missing_test_evidence_is_rejected(self):
        entry = ROW.copy()
        entry["failure_test"] = "tests/regression/missing.c"
        with self.assertRaisesRegex(ValueError, "测试证据"):
            GATE.inventory(self.fixture(entries=[entry]))

    def test_test_path_traversal_is_rejected(self):
        entry = ROW.copy()
        entry["failure_test"] = "tests/../../outside.c"
        with self.assertRaisesRegex(ValueError, "测试证据"):
            GATE.inventory(self.fixture(entries=[entry]))

    def test_missing_owner_and_rationale_are_rejected(self):
        for field in ("owner", "rationale"):
            with self.subTest(field=field):
                row = ROW.copy()
                row[field] = "x"
                with self.assertRaisesRegex(ValueError, "理由"):
                    GATE.inventory(self.fixture(entries=[row]))

    def test_coverage_must_not_claim_unknown_status(self):
        row = ROW.copy()
        row["coverage"] = "unverified"
        with self.assertRaisesRegex(ValueError, "验证等级"):
            GATE.inventory(self.fixture(entries=[row]))

    def test_duplicate_entries_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "重复"):
            GATE.inventory(self.fixture(entries=[ROW.copy(), ROW.copy()]))

    def test_invalid_count_fails_closed(self):
        for value in ("0", "-1", "x", "02"):
            with self.subTest(value=value):
                row = ROW.copy()
                row["count"] = value
                with self.assertRaisesRegex(ValueError, "计数"):
                    GATE.inventory(self.fixture(entries=[row]))

    def test_manifest_schema_change_fails_closed(self):
        root = self.fixture()
        with self.assertRaisesRegex(ValueError, "表头"):
            GATE.inventory(self.write_bad_header(root))

    @staticmethod
    def write_bad_header(root):
        path = root / GATE.MANIFEST
        path.write_text("file\tapi\n", encoding="utf-8")
        return root

    def test_symlink_acquisition_source_is_rejected(self):
        root = self.fixture()
        (root / "src/alias.c").symlink_to(root / "src/sample.c")
        _, errors = GATE.scan(root)
        self.assertTrue(any("符号链接" in item for item in errors), errors)

    def test_missing_production_source_directory_is_rejected(self):
        root = self.fixture()
        (root / "include").rmdir()
        _, errors = GATE.scan(root)
        self.assertTrue(any("源码目录缺失" in item for item in errors))

    def test_unclosed_acquisition_fails_closed(self):
        root = self.fixture("void f(void) { malloc(1; }\n", [])
        _, errors = GATE.scan(root)
        self.assertTrue(any("未闭合" in item for item in errors))


if __name__ == "__main__":
    unittest.main()
