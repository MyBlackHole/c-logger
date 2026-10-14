#!/usr/bin/env python3
"""原始证据归档的恢复正例及损坏/路径负例，不执行 fuzzer。"""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("archive_verify", ROOT / "verify.py")
V = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(V)


class ArchiveTests(unittest.TestCase):
    def test_archive_and_restored_bytes(self):
        originals, decoded, results = V.verify(ROOT)
        self.assertEqual(5, len(results))
        self.assertEqual(156, sum(map(len, decoded.values())))
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "restored"
            V.restore(target, originals, decoded)
            for name, data in originals.items():
                self.assertEqual(data, (target / name).read_bytes())
            for artifact_id, records in decoded.items():
                for name, data in records.items():
                    self.assertEqual(data, (target / artifact_id / name).read_bytes())
            with self.assertRaises(FileExistsError):
                V.restore(target, originals, decoded)

    def test_integrity_and_identity_mutations(self):
        original = json.loads((ROOT / "manifest.json").read_text())
        packed = (ROOT / "artifacts.tar.xz").read_bytes()
        for mutation in ("bundle", "zip", "source", "artifact_id"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                doc = copy.deepcopy(original)
                data = packed
                if mutation == "bundle":
                    data = packed[:100] + bytes([packed[100] ^ 1]) + packed[101:]
                elif mutation == "zip":
                    doc["artifacts"][0]["sha256"] = "0" * 64
                elif mutation == "source":
                    doc["artifacts"][0]["tested_source_sha"] = "0" * 40
                else:
                    doc["artifacts"][0]["artifact_id"] = "../outside"
                (directory / "manifest.json").write_text(json.dumps(doc))
                (directory / "artifacts.tar.xz").write_bytes(data)
                with self.assertRaises(ValueError):
                    V.verify(directory)

    def test_unsafe_paths(self):
        for name in ("../out", "/absolute", "a/../b", "a\\b", "C:drive", "a//b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                V.safe_name(name)


if __name__ == "__main__":
    unittest.main()
