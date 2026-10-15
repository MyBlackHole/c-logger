#!/usr/bin/env python3
"""#211 replay guard tests; fake commands are not Logger validation evidence."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "fuzz_archive_replay", ROOT / "validation/evidence/fuzz-20261014/replay.py")
REPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPLAY)


class ReplayGuards(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="replay-guards-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_restore_compares_original_zip_and_each_member(self):
        restored = self.root / "restored"
        restored.mkdir()
        (restored / "original.zip").write_bytes(b"zip bytes")
        (restored / "123").mkdir()
        (restored / "123/seed").write_bytes(b"input")
        expected = REPLAY.restored_expected({"original.zip": b"zip bytes"},
                                            {"123": {"seed": b"input"}})
        self.assertEqual(expected, REPLAY.snapshot(restored))
        (restored / "123/seed").write_bytes(b"mutation")
        self.assertNotEqual(expected, REPLAY.snapshot(restored))

    def test_missing_and_empty_corpus_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing_corpus"):
            REPLAY.corpus_files(self.root / "absent")
        with self.assertRaisesRegex(ValueError, "empty_or_oversized"):
            REPLAY.corpus_files(self.root)

    def test_corpus_order_is_stable(self):
        for name in ("z", "a", "k"):
            (self.root / name).write_bytes(b"test")
        self.assertEqual(["a", "k", "z"],
                         [p.name for p in REPLAY.corpus_files(self.root)])

    def test_recovered_input_symlink_is_rejected(self):
        (self.root / "file").write_bytes(b"test")
        (self.root / "link").symlink_to(self.root / "file")
        with self.assertRaisesRegex(ValueError, "nonregular"):
            REPLAY.corpus_files(self.root)
        with self.assertRaisesRegex(ValueError, "symlink"):
            REPLAY.snapshot(self.root)

    def test_historical_source_required_and_not_guessed(self):
        source = "a" * 40
        doc = {"schema_version": 1, "artifacts": [
            {"artifact_id": 11896199906, "tested_source_sha": source}]}
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(doc))
        self.assertEqual(source, REPLAY.metadata(self.root, "public-fuzzer")["tested_source_sha"])
        doc["artifacts"][0]["tested_source_sha"] = "main"
        manifest.write_text(json.dumps(doc))
        with self.assertRaisesRegex(ValueError, "invalid_tested_source"):
            REPLAY.metadata(self.root, "public-fuzzer")

    def test_all_recorded_seed_and_selftest_results_are_required(self):
        lines = [f"format PASS seed={s} inputs=512 rejected=0" for s in
                 (1, 107, 20261015, 4294967295)]
        lines += ["format PASS seed=107 inputs=512 rejected=3072"]
        text = "\n".join(lines) + "\n"
        commands = REPLAY.format_commands(text)
        self.assertEqual(["1", "107", "20261015", "4294967295", "selftest"],
                         [seed for seed, _ in commands])
        with self.assertRaises(ValueError):
            REPLAY.format_commands(text.replace("3072", "0"))
        with self.assertRaises(ValueError):
            REPLAY.format_commands("\n".join(lines[:-1]))

    def test_success_retains_command_and_output_identity(self):
        commands = []
        text = REPLAY.run([sys.executable, "-c", "print('test command only')"],
                          self.root, self.root / "run.log", commands)
        self.assertEqual("test command only\n", text)
        self.assertEqual("pass", commands[0]["status"])
        self.assertEqual(REPLAY.sha(text.encode()), commands[0]["log_sha256"])

    def test_nonzero_exit_fails_without_hiding_log(self):
        commands = []
        with self.assertRaisesRegex(ValueError, "command_failed"):
            REPLAY.run([sys.executable, "-c", "print('failure'); raise SystemExit(7)"],
                       self.root, self.root / "failed.log", commands)
        self.assertEqual(7, commands[0]["returncode"])
        self.assertIn("failure", (self.root / "failed.log").read_text())

    def test_timeout_is_not_success(self):
        commands = []
        with self.assertRaisesRegex(ValueError, "timeout"):
            REPLAY.run([sys.executable, "-c", "import time; time.sleep(5)"],
                       self.root, self.root / "timeout.log", commands, timeout=0.1)
        self.assertEqual("timeout", commands[0]["status"])

    def test_existing_log_not_overwritten(self):
        log = self.root / "existing.log"
        log.write_text("keep")
        with self.assertRaises(FileExistsError):
            REPLAY.run([sys.executable, "-c", "print('should not run')"],
                       self.root, log, [])
        self.assertEqual("keep", log.read_text())


if __name__ == "__main__":
    unittest.main()
