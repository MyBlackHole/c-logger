#!/usr/bin/env python3
"""固定速率验收器必须拒绝伪造计数、分位和缺失/重复/损坏输出。"""
import array
import contextlib
import copy
import errno
import gzip
import io
import json
import os
import shlex
import subprocess
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("fixed_rate", ROOT / "scripts/benchmark_fixed_rate.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class Verification(unittest.TestCase):
    def setUp(self):
        self.phases = {1: (array.array("Q", [10, 20, 0]), array.array("Q", [0, 2000000, 0]),
                           array.array("q", [0, -errno.EAGAIN, 0]))}
        self.row = {"raw_schema": 2, "api_success": 1, "api_eagain": 1, "api_other_errors": 0,
                    "api_success_ns": {"count": 1, "p50": 10, "p95": 10, "p99": 10, "max": 10},
                    "phase": 1, "scheduled": 3, "attempted": 2, "generator_missed": 1,
                    "enqueued": 1, "sync_fallbacks": 0, "dropped": 1, "emitted": 1,
                    "failed": 0, "late_over_1ms": 1,
                    "api_ns": {"p50": 10, "p95": 20, "p99": 20, "max": 20},
                    "arrival_late_ns": {"p50": 0, "p95": 2000000, "p99": 2000000, "max": 2000000}}

    def test_wait_status_compatibility(self):
        for code in (0, 1, 42, 255):
            self.assertEqual(bench.wait_exit_code(code << 8), code)
        for signum in (9, 15):
            self.assertEqual(bench.wait_exit_code(signum), -signum)
        with self.assertRaises(ValueError):
            bench.wait_exit_code(0x7f)

    def test_raw_valid(self):
        bench.verify_raw(self.phases, [self.row])

    def test_raw_tamper(self):
        for key in ("scheduled", "attempted", "generator_missed", "enqueued", "dropped", "emitted", "late_over_1ms"):
            row = copy.deepcopy(self.row)
            row[key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                bench.verify_raw(self.phases, [row])
        row = copy.deepcopy(self.row)
        row["api_ns"]["p99"] = 10
        with self.assertRaises(ValueError):
            bench.verify_raw(self.phases, [row])

    def check_file(self, contents):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "output.log"
            path.write_bytes(contents)
            return bench.verify_file(path, self.phases, [self.row], 3)

    def test_file_valid(self):
        self.assertTrue(self.check_file(b"prefix FR phase=1 seq=0 payload=xxx\n")["verified"])

    def test_file_rejects(self):
        line = b"prefix FR phase=1 seq=0 payload=xxx\n"
        for data in (b"", line + line, line[:-1], line.replace(b"xxx", b"xxz"),
                     line.replace(b"seq=0", b"seq=2"), line.replace(b"seq=0", b"seq=3"),
                     line.replace(b"phase=1", b"phase=0")):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.check_file(data)

    def test_raw_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "raw"
            with path.open("wb") as stream:
                stream.write(bench.RAW_MAGIC)
                array.array("Q", [1, 3]).tofile(stream)
                for values in self.phases[1]:
                    values.tofile(stream)
            self.assertEqual(bench.read_raw(path), self.phases)
            path.write_bytes(b"bad")
            with self.assertRaises(ValueError):
                bench.read_raw(path)

    def test_status_counts_and_success_percentiles(self):
        for key in ("api_success", "api_eagain", "api_other_errors"):
            row = copy.deepcopy(self.row)
            row[key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "raw_status"):
                bench.verify_raw(self.phases, [row])
        for key in ("count", "p50", "p95", "p99", "max"):
            row = copy.deepcopy(self.row)
            row["api_success_ns"][key] += 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                bench.verify_raw(self.phases, [row])

    def test_other_status_never_becomes_valid_sample(self):
        for code in (-errno.EIO, -errno.ENODEV, 1):
            phases = copy.deepcopy(self.phases)
            phases[1][2][1] = code
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, "raw_status"):
                bench.verify_raw(phases, [self.row])

    def test_unattempted_status_or_lateness_rejected(self):
        for index in (1, 2):
            phases = copy.deepcopy(self.phases)
            phases[1][index][2] = 1
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "unattempted"):
                bench.verify_raw(phases, [self.row])

    def test_zero_success_has_no_latency(self):
        phases = copy.deepcopy(self.phases)
        phases[1][2][0] = -errno.EAGAIN
        row = copy.deepcopy(self.row)
        row.update(api_success=0, api_eagain=2, enqueued=0, dropped=2, emitted=0)
        row["api_success_ns"] = {"count": 0, "p50": None, "p95": None, "p99": None, "max": None}
        bench.verify_raw(phases, [row])
        row["api_success_ns"]["p99"] = 0
        with self.assertRaises(ValueError):
            bench.verify_raw(phases, [row])

    def test_output_cannot_substitute_dropped_slot(self):
        with self.assertRaisesRegex(ValueError, "file_record_not_successful"):
            self.check_file(b"prefix FR phase=1 seq=1 payload=xxx\n")

    def test_raw_schema_and_truncation(self):
        header = array.array("Q", [1, 3]).tobytes()
        body = b"".join(a.tobytes() for a in self.phases[1])
        valid = bench.RAW_MAGIC + header + body
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "raw"
            cases = (header + body, bench.RAW_MAGIC, valid[:-1], valid[:35],
                     valid + b"x", valid + header + body,
                     bench.RAW_MAGIC + array.array("Q", [1, 0]).tobytes())
            for data in cases:
                path.write_bytes(data)
                with self.subTest(length=len(data)), self.assertRaises(ValueError):
                    bench.read_raw(path)
        with self.assertRaisesRegex(ValueError, "raw_phase_mismatch"):
            bench.verify_raw(self.phases, [self.row, self.row])

    def test_invalid_child_receipts_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            owned = root / "temporary"
            owned.mkdir()
            for variant in ("healthy", "diagnostic", "close-error", "api-error", "bad-json"):
                binary, case, originals = self.write_child_fixture(root, variant)
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    result = bench.run_sample(binary, root / variant, case, 2, owned)
                with self.subTest(variant=variant):
                    self.assertEqual(result["verification_valid"], variant == "healthy", result)
                    for name, contents in originals.items():
                        archive = root / variant / (name + ".gz")
                        self.assertEqual(gzip.decompress(archive.read_bytes()), contents)
                        self.assertFalse(archive.with_name(archive.name + ".partial").exists())
                    self.assertEqual(list(owned.iterdir()), [])
                    self.assertEqual(stderr.getvalue(), "")

    def write_child_fixture(self, root, variant="healthy"):
        row = dict(copy.deepcopy(self.row), valid=True)
        arrays = copy.deepcopy(self.phases[1])
        close = {"close_rc": 0, "close_errno": 0}
        diagnostic = "runtime error: synthetic\n" if variant == "diagnostic" else ""
        if variant == "close-error":
            close = {"close_rc": -errno.EIO, "close_errno": errno.EIO}
        if variant == "api-error":
            arrays[2][1] = -errno.EIO
            row.update(api_eagain=0, api_other_errors=1)
        rows = [{"clock_pair_ns": {"p50": 1, "p99": 2}}, row, close]
        if variant == "bad-json":
            rows[0] = []
        raw = bench.RAW_MAGIC + array.array("Q", [1, 3]).tobytes()
        raw += b"".join(a.tobytes() for a in arrays)
        log = b"prefix FR phase=1 seq=0 payload=xxx\n"
        output = "".join(json.dumps(value) + "\n" for value in rows)
        script = ("#!/usr/bin/env python3\nimport pathlib,sys\n"
                  f"pathlib.Path(sys.argv[7]).write_bytes(bytes.fromhex({raw.hex()!r}))\n"
                  f"pathlib.Path(sys.argv[6]).write_bytes({log!r})\n"
                  f"sys.stdout.write({output!r})\nsys.stderr.write({diagnostic!r})\n")
        binary = root / "fixture"
        binary.write_text(script)
        binary.chmod(0o755)
        case = {"threads": 1, "rate": 3, "seconds": 1, "warmup": 0,
                "payload_bytes": 3, "backend": "file", "repeat": 0}
        return binary, case, {"latency.bin": raw, "output.log": log}

    def test_archive_failures_preserve_complete_originals(self):
        cases = (("latency.bin", "write", "healthy"),
                 ("output.log", "write", "healthy"),
                 ("output.log", "write", "diagnostic"),
                 ("latency.bin", "close", "healthy"))
        for source_name, stage, variant in cases:
            with self.subTest(source=source_name, stage=stage, variant=variant), \
                    tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                binary, case, originals = self.write_child_fixture(root, variant)
                output, owned = root / "result", root / "temporary"
                owned.mkdir()
                artifact = source_name + ".gz"
                real_copy, real_close = bench.shutil.copyfileobj, bench.gzip.GzipFile.close

                def fail_copy(src, dst, *args):
                    if stage == "write" and Path(src.name).name == source_name:
                        dst.write(src.read(7))
                        raise OSError(errno.ENOSPC, "controlled_archive_write")
                    return real_copy(src, dst, *args)

                def fail_close(dst):
                    fail = (stage == "close" and dst.fileobj is not None and
                            Path(dst.name).name in (artifact, artifact + ".partial"))
                    real_close(dst)
                    if fail:
                        raise OSError(errno.ENOSPC, "controlled_archive_close")

                stderr = io.StringIO()
                with mock.patch.object(bench.shutil, "copyfileobj", fail_copy), \
                        mock.patch.object(bench.gzip.GzipFile, "close", fail_close), \
                        contextlib.redirect_stderr(stderr):
                    result = bench.run_sample(binary, output, case, 2, owned)
                self.assertFalse(result["verification_valid"])
                self.assertEqual(result["archive_error"]["artifact"], artifact)
                self.assertEqual(result["archive_error"]["errno"], errno.ENOSPC)
                self.assertIn("controlled_archive_" + stage, result["archive_error"]["message"])
                if variant == "diagnostic":
                    self.assertEqual(result["verification_error"], "child_run_invalid")
                else:
                    self.assertNotIn("verification_error", result)
                retained = Path(result["retained_temp_dir"])
                self.assertTrue(retained.is_absolute())
                self.assertEqual(retained.parent, owned.resolve())
                self.assertEqual(list(owned.iterdir()), [retained])
                for name, contents in originals.items():
                    self.assertEqual((retained / name).read_bytes(), contents)
                self.assertFalse((output / artifact).exists())
                self.assertTrue((output / (artifact + ".partial")).is_file())
                if source_name == "output.log":
                    self.assertEqual(gzip.decompress((output / "latency.bin.gz").read_bytes()),
                                     originals["latency.bin"])
                self.assertEqual(json.loads((output / "result.json").read_text()),
                                 json.loads(json.dumps(result)))
                receipts = [json.loads(line) for line in stderr.getvalue().splitlines()]
                self.assertTrue(any(row.get("retained_temp_dir") == str(retained) for row in receipts))

    def test_result_write_failure_reports_and_preserves_originals(self):
        for variant in ("healthy", "diagnostic"):
            with self.subTest(variant=variant), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                binary, case, originals = self.write_child_fixture(root, variant)
                output, owned = root / "result", root / "temporary"
                owned.mkdir()
                real_write, real_compress = Path.write_text, bench.compress

                def fail_result(path, *args, **kwargs):
                    if path == output / "result.json":
                        raise OSError(errno.ENOSPC, "controlled_result_write")
                    return real_write(path, *args, **kwargs)

                def archive(src, target):
                    if variant == "diagnostic":
                        raise OSError(errno.ENOSPC, "controlled_archive_write")
                    return real_compress(src, target)

                stderr = io.StringIO()
                with mock.patch.object(Path, "write_text", fail_result), \
                        mock.patch.object(bench, "compress", archive), \
                        contextlib.redirect_stderr(stderr), self.assertRaises(OSError) as raised:
                    bench.run_sample(binary, output, case, 2, owned)
                self.assertEqual(raised.exception.errno, errno.ENOSPC)
                receipts = [json.loads(line) for line in stderr.getvalue().splitlines()]
                retained_rows = [row for row in receipts if "retained_temp_dir" in row]
                self.assertEqual(len(retained_rows), 1)
                receipt = retained_rows[0]
                self.assertFalse(receipt["verification_valid"])
                self.assertEqual(receipt["case"], case)
                self.assertEqual(receipt["returncode"], 0)
                if variant == "diagnostic":
                    self.assertEqual(receipt["verification_error"], "child_run_invalid")
                    self.assertEqual(receipt["archive_error"]["artifact"], "latency.bin.gz")
                    self.assertEqual(receipt["archive_error"]["errno"], errno.ENOSPC)
                    self.assertIn("controlled_archive_write", receipt["archive_error"]["message"])
                retained = Path(receipt["retained_temp_dir"])
                self.assertTrue(retained.is_absolute())
                self.assertEqual(retained.parent, owned.resolve())
                self.assertEqual(list(owned.iterdir()), [retained])
                for name, contents in originals.items():
                    self.assertEqual((retained / name).read_bytes(), contents)
                    if variant == "healthy":
                        self.assertEqual(gzip.decompress((output / (name + ".gz")).read_bytes()), contents)
                self.assertFalse((output / "result.json").exists())
                self.assertTrue((output / "stdout.jsonl").is_file())
                self.assertTrue((output / "stderr.log").is_file())

    def test_cleanup_failure_is_recorded_in_summary_and_stops(self):
        # 使用真实子进程生成完整原件，分别模拟部分删除和已有无效样本。
        for variant, partial, handled in (("healthy", True, False),
                                          ("diagnostic", False, False),
                                          ("healthy", False, True)):
            with self.subTest(variant=variant, handled=handled), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                binary, case, originals = self.write_child_fixture(root, variant)
                library, build_log = root / "library", root / "build.log"
                library.write_bytes(b"controlled_library\n")
                build_log.write_text("controlled_build\n")
                output, owned = root / "evidence", root / "temporary"
                owned.mkdir()
                removals = []

                def fail_cleanup(path):
                    removals.append(Path(path))
                    if partial:
                        (Path(path) / "latency.bin").unlink()
                    raise OSError(errno.ENOTEMPTY, "controlled_cleanup_failure")

                argv = ["benchmark_fixed_rate.py", "--binary", str(binary),
                        "--library", str(library), "--build-log", str(build_log),
                        "--out", str(output), "--threads", "1", "--rates", "3",
                        "--payload-bytes", "3", "--backends", "file", "--seconds", "1",
                        "--warmup", "0", "--repeats", "2", "--temp-root", str(owned)]
                stdout, stderr = io.StringIO(), io.StringIO()
                with mock.patch.object(bench, "ROOT", root), \
                        mock.patch.object(bench, "command", return_value={"returncode": 0, "stdout": "", "stderr": ""}), \
                        mock.patch.object(bench.shutil, "rmtree", fail_cleanup), \
                        mock.patch.object(bench.signal, "signal"), \
                        mock.patch.object(sys, "argv", argv), \
                        contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    if handled:
                        # 调用方已经处理的异常不属于 run_sample 待传播错误。
                        try:
                            raise ValueError("controlled_caller_exception")
                        except ValueError:
                            rc = bench.main()
                    else:
                        rc = bench.main()
                self.assertEqual(rc, 1)
                self.assertEqual(len(removals), 1)
                self.assertFalse((output / "sample-001").exists())
                sample = output / "sample-000"
                result = json.loads((sample / "result.json").read_text())
                rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
                self.assertEqual(rows, [result])
                self.assertFalse(result["verification_valid"])
                self.assertEqual(result["case"], case)
                self.assertEqual(result["returncode"], 0)
                self.assertFalse(result["timed_out"])
                self.assertEqual(result["cleanup_error"]["errno"], errno.ENOTEMPTY)
                self.assertIn("controlled_cleanup_failure", result["cleanup_error"]["message"])
                self.assertEqual(result["cleanup_error"]["path"], str(removals[0]))
                self.assertEqual(result["retained_temp_dir"], str(removals[0]))
                self.assertNotIn("archive_error", result)
                if variant == "diagnostic":
                    self.assertEqual(result["verification_error"], "child_run_invalid")
                else:
                    self.assertNotIn("verification_error", result)
                for name, contents in originals.items():
                    self.assertEqual(gzip.decompress((sample / (name + ".gz")).read_bytes()), contents)
                    if name != "latency.bin" or not partial:
                        self.assertEqual((removals[0] / name).read_bytes(), contents)
                self.assertEqual((removals[0] / "latency.bin").exists(), not partial)
                receipts = [json.loads(line) for line in stderr.getvalue().splitlines()]
                self.assertEqual(len(receipts), 1)
                self.assertEqual(receipts[0]["error"], "sample_evidence_retained")
                self.assertEqual(receipts[0]["cleanup_error"], result["cleanup_error"])
                self.assertFalse(receipts[0]["verification_valid"])
                progress = [json.loads(line) for line in stdout.getvalue().splitlines()]
                self.assertEqual(len(progress), 1)
                self.assertFalse(progress[0]["verification_valid"])

    def test_cleanup_receipt_write_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary, case, originals = self.write_child_fixture(root)
            output, owned = root / "result", root / "temporary"
            owned.mkdir()
            real_write = Path.write_text
            writes = []

            def fail_rewrite(path, *args, **kwargs):
                if path == output / "result.json":
                    writes.append(path)
                    if len(writes) == 2:
                        raise OSError(errno.ENOSPC, "controlled_cleanup_receipt_write")
                return real_write(path, *args, **kwargs)

            stderr = io.StringIO()
            with mock.patch.object(bench.shutil, "rmtree", side_effect=OSError(errno.ENOTEMPTY, "controlled_cleanup_failure")), \
                    mock.patch.object(Path, "write_text", fail_rewrite), \
                    contextlib.redirect_stderr(stderr), self.assertRaises(OSError) as raised:
                bench.run_sample(binary, output, case, 2, owned)
            self.assertEqual(raised.exception.errno, errno.ENOSPC)
            self.assertEqual(len(writes), 2)
            receipts = [json.loads(line) for line in stderr.getvalue().splitlines()]
            self.assertEqual(len(receipts), 1)
            receipt = receipts[0]
            self.assertFalse(receipt["verification_valid"])
            self.assertEqual(receipt["cleanup_error"]["errno"], errno.ENOTEMPTY)
            retained = Path(receipt["retained_temp_dir"])
            for name, contents in originals.items():
                self.assertEqual((retained / name).read_bytes(), contents)
                self.assertEqual(gzip.decompress((output / (name + ".gz")).read_bytes()), contents)

    def test_cleanup_error_without_directory_has_no_retained_locator(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary, case, originals = self.write_child_fixture(root)
            output, owned = root / "result", root / "temporary"
            owned.mkdir()
            real_remove = bench.shutil.rmtree

            def removed_elsewhere(path):
                real_remove(path)
                raise OSError(errno.ENOENT, "controlled_directory_disappeared")

            stderr = io.StringIO()
            with mock.patch.object(bench.shutil, "rmtree", removed_elsewhere), \
                    contextlib.redirect_stderr(stderr):
                result = bench.run_sample(binary, output, case, 2, owned)
            self.assertFalse(result["verification_valid"])
            self.assertEqual(result["cleanup_error"]["errno"], errno.ENOENT)
            self.assertNotIn("retained_temp_dir", result)
            self.assertEqual(list(owned.iterdir()), [])
            receipt = json.loads(stderr.getvalue())
            self.assertEqual(receipt["error"], "sample_cleanup_failed")
            self.assertNotIn("retained_temp_dir", receipt)
            for name, contents in originals.items():
                self.assertEqual(gzip.decompress((output / (name + ".gz")).read_bytes()), contents)

    def test_cleanup_inspection_error_preserves_pending_receipt_error(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary, case, originals = self.write_child_fixture(root)
            output, owned = root / "result", root / "temporary"
            owned.mkdir()
            real_write, real_iterdir = Path.write_text, Path.iterdir
            writes = []

            def fail_result(path, *args, **kwargs):
                if path == output / "result.json":
                    writes.append(path)
                    raise OSError(errno.ENOSPC, "controlled_result_write")
                return real_write(path, *args, **kwargs)

            def fail_inspection(path):
                if path.parent == owned:
                    raise OSError(errno.EACCES, "controlled_cleanup_inspection")
                return real_iterdir(path)

            stderr = io.StringIO()
            with mock.patch.object(Path, "write_text", fail_result), \
                    mock.patch.object(Path, "iterdir", fail_inspection), \
                    contextlib.redirect_stderr(stderr), self.assertRaises(OSError) as raised:
                bench.run_sample(binary, output, case, 2, owned)
            self.assertEqual(raised.exception.errno, errno.ENOSPC)
            self.assertEqual(len(writes), 1)
            receipt = json.loads(stderr.getvalue())
            self.assertFalse(receipt["verification_valid"])
            self.assertEqual(receipt["cleanup_error"]["operation"], "inspect_temp_dir")
            self.assertEqual(receipt["cleanup_error"]["errno"], errno.EACCES)
            retained = Path(receipt["retained_temp_dir"])
            for name, contents in originals.items():
                self.assertEqual((retained / name).read_bytes(), contents)
                self.assertEqual(gzip.decompress((output / (name + ".gz")).read_bytes()), contents)

    def test_timeout_cleans_own_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "blocked"
            binary.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
            binary.chmod(0o755)
            owned = root / "temporary"
            owned.mkdir()
            case = {"threads": 1, "rate": 1, "seconds": 1, "warmup": 0,
                    "payload_bytes": 3, "backend": "file", "repeat": 0}
            result = bench.run_sample(binary, root / "result", case, 0.1, owned)
            self.assertTrue(result["timed_out"])
            self.assertFalse(result["verification_valid"])
            self.assertEqual(list(owned.iterdir()), [])
            self.assertTrue((root / "result/stdout.jsonl").exists())

    def test_runner_exception_reaps_child(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "blocked"
            binary.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n")
            binary.chmod(0o755)
            owned = root / "temporary"
            owned.mkdir()
            case = {"threads": 1, "rate": 1, "seconds": 1, "warmup": 0,
                    "payload_bytes": 3, "backend": "file", "repeat": 0}
            original = bench.os.wait4
            calls = []
            def interrupted(pid, flags):
                calls.append(pid)
                if len(calls) == 1:
                    raise KeyboardInterrupt()
                return original(pid, flags)
            with mock.patch.object(bench.os, "wait4", side_effect=interrupted):
                result = bench.run_sample(binary, root / "result", case, 1, owned)
            self.assertEqual(result["runner_error"], "KeyboardInterrupt")
            self.assertFalse(result["verification_valid"])
            self.assertEqual(list(owned.iterdir()), [])
            with self.assertRaises(ProcessLookupError):
                bench.os.kill(calls[0], 0)


class EvidenceBudget(unittest.TestCase):
    def run_case(self, placement, repeats=1):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root, limit = Path(temporary.name), 16384
        binary, library, build_log = [root / name for name in ("binary", "library", "build.log")]
        for path in (binary, library, build_log):
            path.write_bytes(b"fixture\n")
        if placement == "metadata":
            build_log.write_bytes(b"x" * limit)
        output, completed = root / "evidence", []
        real_read = Path.read_text

        def small_environment(path, *args, **kwargs):
            if str(path) in ("/proc/cpuinfo", "/proc/mounts"):
                return "controlled_environment\n"
            return real_read(path, *args, **kwargs)

        def directory_bytes():
            return sum(path.stat().st_size for path in output.rglob("*") if path.is_file())

        def sample(binary, sample_out, case, timeout, temp_root):
            # 只构造证据目录边界；不以缩小子进程 RLIMIT 来制造失败样本。
            sample_out.mkdir()
            result = {"case": case, "verification_valid": True}
            (sample_out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            line_bytes = len((json.dumps(result) + "\n").encode())
            target = limit + (1 if placement == "sample" else 0)
            if placement == "exact":
                target -= line_bytes
            padding = target - directory_bytes()
            self.assertGreater(padding, 0)
            (sample_out / "controlled.bin").write_bytes(b"x" * padding)
            completed.append({"before_summary": directory_bytes(), "summary_bytes": line_bytes})
            return result

        argv = ["benchmark_fixed_rate.py", "--binary", str(binary), "--library", str(library),
                "--build-log", str(build_log), "--out", str(output), "--threads", "1",
                "--rates", "1", "--payload-bytes", "1", "--backends", "null",
                "--seconds", "1", "--warmup", "0", "--repeats", str(repeats)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(bench, "MAX_BYTES", limit), mock.patch.object(bench, "ROOT", root), \
                mock.patch.object(bench, "command", return_value={"returncode": 0, "stdout": "", "stderr": ""}), \
                mock.patch.object(bench, "run_sample", side_effect=sample) as runner, \
                mock.patch.object(Path, "read_text", small_environment), \
                mock.patch.object(bench.signal, "signal"), mock.patch.object(sys, "argv", argv), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            rc = bench.main()
        return output, rc, runner.call_count, completed, limit

    def check_sample_failure(self, placement, repeats=1):
        output, rc, calls, completed, limit = self.run_case(placement, repeats)
        self.assertEqual(rc, 1)
        self.assertEqual(calls, 1)
        self.assertFalse((output / "sample-001").exists())
        failure = json.loads((output / "failure.json").read_text())
        self.assertFalse(failure["verification_valid"])
        self.assertEqual(failure["error"], "evidence_budget_exceeded")
        self.assertEqual(failure["limit_bytes"], limit)
        self.assertEqual(failure["observed_bytes"],
                         completed[0]["before_summary"] + completed[0]["summary_bytes"])
        self.assertGreater(failure["observed_bytes"], limit)
        result = json.loads((output / "sample-000/result.json").read_text())
        rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
        self.assertEqual(rows, [result])
        self.assertFalse(result["verification_valid"])
        self.assertEqual(result["evidence_error"], "evidence_budget_exceeded")

    def test_last_sample_exceeding_budget_is_invalid(self):
        self.check_sample_failure("sample")

    def test_summary_line_can_exceed_budget(self):
        self.check_sample_failure("summary")

    def test_budget_failure_stops_later_samples(self):
        self.check_sample_failure("sample", repeats=2)

    def test_metadata_exceeding_budget_starts_no_samples(self):
        output, rc, calls, completed, limit = self.run_case("metadata")
        self.assertEqual(rc, 1)
        self.assertEqual(calls, 0)
        self.assertEqual(completed, [])
        self.assertFalse((output / "results.jsonl").exists())
        self.assertFalse((output / "sample-000").exists())
        failure = json.loads((output / "failure.json").read_text())
        self.assertEqual(failure["error"], "evidence_budget_exceeded")
        self.assertFalse(failure["verification_valid"])
        self.assertEqual(failure["limit_bytes"], limit)
        self.assertGreater(failure["observed_bytes"], limit)

    def test_exact_budget_remains_valid(self):
        output, rc, calls, completed, limit = self.run_case("exact")
        self.assertEqual(rc, 0)
        self.assertEqual(calls, 1)
        self.assertEqual(completed[0]["before_summary"] + completed[0]["summary_bytes"], limit)
        self.assertEqual(sum(path.stat().st_size for path in output.rglob("*") if path.is_file()), limit)
        self.assertFalse((output / "failure.json").exists())
        result = json.loads((output / "sample-000/result.json").read_text())
        rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
        self.assertEqual(rows, [result])
        self.assertTrue(result["verification_valid"])


PRODUCER_FIXTURE = r'''#define main unused_benchmark_main
#define logger_log injected_logger_log
#include "driver.c"
#undef logger_log
#undef main
static const int results[] = { 0, -EAGAIN, -EIO };
static unsigned used;
int injected_logger_log(logger_t *l, logger_level_t level, const char *module,
                        const char *file, int line, const char *func,
                        const char *fmt, ...)
{
    (void)l; (void)level; (void)module; (void)file;
    (void)line; (void)func; (void)fmt;
    if (used >= 3) abort();
    return results[used++];
}
int main(void)
{
    uint64_t calls[3] = {0}, late[3] = {0};
    int64_t status[3] = {0};
    struct phase p = { .payload = "x", .rate = 1000000, .count = 3,
        .threads = 1, .number = 1, .ready = 1, .calls = calls, .late = late,
        .status = status, .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER };
    struct producer producer = { .phase = &p };
    p.start = now_ns();
    p.end = p.start + 5000000000ull;
    produce(&producer);
    if (used != 3 || producer.attempted != 3 || producer.error) return 2;
    for (unsigned i = 0; i < 3; ++i) {
        if (!calls[i] || status[i] != results[i]) {
            puts("producer_status_not_recorded");
            return 1;
        }
    }
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.mutex);
    puts("producer_status_recorded");
    return 0;
}
'''

class ProducerStatus(unittest.TestCase):
    def test_real_producer_and_lost_status_mutation(self):
        source = (ROOT / "tests/bench_fixed_rate.c").read_text()
        anchor = "p->status[i] = status;"
        self.assertEqual(source.count(anchor), 1)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        env = dict(os.environ, LC_ALL="C")
        version = subprocess.run(compiler + ["--version"], capture_output=True,
                                 text=True, timeout=20, env=env, check=True)
        print("FIXED_RATE_STATUS_COMPILER=" + version.stdout.splitlines()[0], flush=True)
        with tempfile.TemporaryDirectory(prefix="fixed-rate-status-") as temp:
            work = Path(temp)
            (work / "probe.c").write_text(PRODUCER_FIXTURE)
            for lost in (False, True):
                # 只在临时副本变异；生产驱动和构建目标不被改写。
                (work / "driver.c").write_text(source.replace(anchor, "p->status[i] = 0; (void)status;") if lost else source)
                cmd = compiler + ["-std=gnu11", "-O0", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                                  "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                                  "-I" + str(ROOT / "include"), str(work / "probe.c"),
                                  "-pthread", "-o", str(work / "probe")]
                built = subprocess.run(cmd, capture_output=True, text=True, timeout=20, env=env)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([str(work / "probe")], capture_output=True, text=True,
                                        timeout=10, env=env)
                self.assertEqual(result.returncode, 1 if lost else 0, result.stderr)
                self.assertEqual(result.stdout, "producer_status_not_recorded\n" if lost else "producer_status_recorded\n")
                self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
