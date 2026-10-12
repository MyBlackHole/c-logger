#!/usr/bin/env python3
"""PR #116 对照判定器：正常、缺失、错误口径与过载分类的独立用例。"""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "compare_pr116_production.py"
spec = importlib.util.spec_from_file_location("pr116_compare", SCRIPT)
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)

CASE = ("常规/1线程/64字节", 1, 64, 65536, 100)


def sample():
    row = {key: 0 for key in compare.FIELDS}
    row.update(
        threads=1, records_per_thread=100, payload_bytes=64,
        queue_capacity=65536, attempted=100, enqueued=100,
        emitted=100, failed=0, dropped=0, sync_fallbacks=0,
        queue_storage_bytes=1024, api_p50_ns=10,
        api_p95_ns=15, api_p99_ns=20, producer_seconds=0.1,
        end_to_end_seconds=0.2, producer_logs_per_sec=1000.0,
        end_to_end_logs_per_sec=500.0, valid=True,
    )
    return row


class Pr116DirectTests(unittest.TestCase):
    def test_valid(self):
        compare.validate_sample(sample(), CASE)

    def test_reject_missing_and_invalid_percentiles(self):
        for key in ("queue_storage_bytes", "api_p99_ns", "emitted"):
            with self.subTest(key=key):
                row = sample()
                del row[key]
                with self.assertRaises(ValueError):
                    compare.validate_sample(row, CASE)
        row = sample()
        row["api_p99_ns"] = 9
        with self.assertRaisesRegex(ValueError, "percentile"):
            compare.validate_sample(row, CASE)

    def test_reject_accounting_and_wrong_workload(self):
        for field, value in (
            ("attempted", 99), ("dropped", 1), ("failed", 1),
            ("threads", 4), ("valid", False),
        ):
            with self.subTest(field=field):
                row = sample()
                row[field] = value
                with self.assertRaises(ValueError):
                    compare.validate_sample(row, CASE)

    def test_overload_is_not_speedup(self):
        before = sample()
        after = sample()
        self.assertTrue(compare.clean_pair(before, after))
        after["dropped"] = 4
        self.assertFalse(compare.clean_pair(before, after))
        after["dropped"] = 0
        after["sync_fallbacks"] = 1
        self.assertFalse(compare.clean_pair(before, after))

    def test_summary_classification(self):
        meta = {"repeats": 2}
        entries = []
        for repeat in range(2):
            for variant in ("before", "after"):
                row = sample()
                row.update(case=CASE[0], variant=variant,
                           round=repeat, warmup=False)
                if repeat == 1 and variant == "after":
                    row["dropped"] = 1
                    row["enqueued"] = 99
                    row["emitted"] = 99
                entries.append(row)
        summary = compare.summarize(entries, [CASE[0]], meta)
        self.assertIn("1/2", summary)
        self.assertIn(compare.BEFORE, summary)
        self.assertIn(compare.AFTER, summary)


if __name__ == "__main__":
    unittest.main()
