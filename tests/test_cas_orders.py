#!/usr/bin/env python3
"""Positive and negative CAS gate fixtures; never mutate the working tree."""
from __future__ import annotations

import csv
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/check_cas_orders.py"
# Keep source-tree imports read-only during the Xmake test run.
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("cas_gate", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class CasOrderTests(unittest.TestCase):
    def fixture(self):
        temp = tempfile.TemporaryDirectory(prefix="logger-cas-orders-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        for directory in ("src", "include"):
            shutil.copytree(ROOT / directory, root / directory)
        (root / "validation").mkdir()
        shutil.copyfile(ROOT / "validation/cas_orders.tsv",
                        root / "validation/cas_orders.tsv")
        return root

    def run_gate(self, root, expected, diagnostic=""):
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertIn(diagnostic, result.stdout + result.stderr)

    def replace(self, root, old, new, filename="src/logger_global.c"):
        path = root / filename
        source = path.read_text()
        self.assertEqual(source.count(old), 1, old)
        path.write_text(source.replace(old, new))

    def test_checked_in_inventory(self):
        self.run_gate(ROOT, 0, "reviewed calls passed")

    def test_each_global_lifecycle_regression_is_rejected(self):
        calls = {
            "start": "&g_ticket, &old, ticket,",
            "publish": "&g_ticket, &ticket, with_phase(ticket, G_RUNNING),",
            "stop": "&g_ticket, &current, stopping,",
            "stopped": "&g_ticket, &stopping, with_phase(current, G_STOPPED),",
        }
        for name, args in calls.items():
            with self.subTest(transition=name):
                root = self.fixture()
                old = args + "\n\t\t    memory_order_acq_rel, memory_order_acquire"
                self.replace(root, old, old.replace("memory_order_acq_rel", "memory_order_release"))
                self.run_gate(root, 1, "unapproved C11 order pair release/acquire")

    def test_order_pair_matrix(self):
        # Independent expected contract, including failure release/acq_rel rejects.
        allowed = {"relaxed/relaxed", "consume/relaxed", "consume/consume",
                   "acquire/relaxed", "acquire/consume", "acquire/acquire",
                   "release/relaxed", "acq_rel/relaxed", "acq_rel/consume",
                   "acq_rel/acquire", "seq_cst/relaxed", "seq_cst/consume",
                   "seq_cst/acquire", "seq_cst/seq_cst"}
        root = self.fixture()
        for directory in ("src", "include"):
            shutil.rmtree(root / directory)
            (root / directory).mkdir()
        for success in gate.ALLOWED_FAILURES:
            for failure in gate.ALLOWED_FAILURES:
                with self.subTest(success=success, failure=failure):
                    (root / "src/probe.c").write_text(
                        "atomic_compare_exchange_strong_explicit(&a, &e, 1, "
                        f"memory_order_{success}, memory_order_{failure});\n")
                    _, errors = gate.scan(root)
                    self.assertEqual(not errors, f"{success}/{failure}" in allowed, errors)

    def test_legal_but_unreviewed_order_change(self):
        root = self.fixture()
        self.replace(root, "memory_order_release, memory_order_relaxed);",
                     "memory_order_seq_cst, memory_order_relaxed);", "src/logger_worker.c")
        self.run_gate(root, 1, "unreviewed/changed CAS")

    def test_extra_and_duplicate_calls(self):
        for filename, call in (
            ("include/extra.h", "atomic_compare_exchange_weak_explicit(&new, &old, 1, "
                                 "memory_order_relaxed, memory_order_relaxed);"),
            ("src/console.c", "atomic_compare_exchange_weak_explicit(&g_config, &old, next, "
                               "memory_order_relaxed, memory_order_relaxed);"),
        ):
            with self.subTest(filename=filename):
                root = self.fixture()
                with (root / filename).open("a") as stream:
                    stream.write("\n" + call + "\n")
                self.run_gate(root, 1, "unreviewed/changed CAS")

    def test_removed_source_is_rejected(self):
        root = self.fixture()
        (root / "src/console.c").unlink()
        self.run_gate(root, 1, "missing/changed CAS")

    def test_missing_or_invalid_inventory(self):
        for contents in ("", "file\toperation\n", "delete"):
            with self.subTest(contents=contents):
                root = self.fixture()
                path = root / "validation/cas_orders.tsv"
                if contents == "delete":
                    path.unlink()
                else:
                    path.write_text(contents)
                self.run_gate(root, 1)

    def test_invalid_pair_cannot_be_allowlisted(self):
        root = self.fixture()
        path = root / "validation/cas_orders.tsv"
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        rows[0]["failure"] = "release"
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=gate.FIELDS, delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        self.run_gate(root, 1, "unapproved CAS operation/order pair")

    def test_aliases_and_implicit_or_builtin_cas_are_rejected(self):
        for call in ("atomic_compare_exchange_strong(&a, &e, 1);",
                     "__atomic_compare_exchange_n(&a, &e, 1, 0, 0, 0);",
                     "__c11_atomic_compare_exchange_strong(&a, &e, 1, 0, 0);",
                     "__sync_bool_compare_and_swap(&a, 0, 1);",
                     "#define CAS atomic_compare_exchange_strong_explicit",
                     "atomic_compare_exchange_strong_explicit(&a, &e, 1, ORDER, ORDER);"):
            with self.subTest(call=call):
                root = self.fixture()
                (root / "include/probe.h").write_text(call)
                self.run_gate(root, 1)

    def test_comments_literals_and_whitespace_are_ignored(self):
        root = self.fixture()
        with (root / "src/console.c").open("a") as stream:
            stream.write('\n/* atomic_compare_exchange_strong(&a, &e, 1); */\n'
                         '// atomic_compare_exchange_strong(&a, &e, 1);\n'
                         'const char *s = "atomic_compare_exchange_strong(&a, &e, 1)";\n')
        self.replace(root, "&g_ticket, &old, ticket,", "&g_ticket, /* review */ &old,\n ticket,")
        self.run_gate(root, 0)

    def test_nested_call_arguments(self):
        self.assertEqual(gate.arguments("( &a[i + 1], &e, f(x, y), "
                                       "memory_order_acq_rel, memory_order_acquire)", 0),
                         ["&a[i+1]", "&e", "f(x,y)",
                          "memory_order_acq_rel", "memory_order_acquire"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
