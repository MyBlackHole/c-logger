#!/usr/bin/env python3
"""以实际构建命令、ELF 依赖和小样本检查基准链接边界，不运行性能压力矩阵。"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def commands(log, target, compile_only):
    result = []
    for line in ANSI.sub("", log).splitlines():
        if " -o " not in line:
            continue
        args = shlex.split(line)
        output = args[args.index("-o") + 1]
        if compile_only and "-c" in args and f"/.objs/{target}/" in output:
            result.append(args)
        elif not compile_only and "-c" not in args and Path(output).name == target:
            result.append(args)
    return result


def verify_production(log, targets):
    library = commands(log, "logger", True)
    assert library, "missing production compilation commands"
    for args in library:
        assert "-O3" in args and "-DNDEBUG" in args, args
        assert "-DLOGGER_ENABLE_LOCKDEP=0" in args, args
        assert "-DLOGGER_ENABLE_FAULT_INJECTION=0" in args, args
        assert "-D_FORTIFY_SOURCE=0" not in args, args
        assert not any(a.startswith("-O") and a != "-O3" for a in args), args
    for target in targets:
        drivers = commands(log, target, True)
        assert len(drivers) == 1, (target, drivers)
        assert "-O3" in drivers[0] and "-DNDEBUG" in drivers[0], drivers
        links = commands(log, target, False)
        assert len(links) == 1, (target, links)
        assert "-llogger" in links[0], links
        assert not any("logger_regression_support" in a or "logger_test_support" in a
                       for a in links[0]), links


class BenchmarkBuild(unittest.TestCase):
    def test_comparison_build_mismatch(self):
        # 固定计数样本隔离比较脚本逻辑；不以不稳定的吞吐实测验证元数据。
        with tempfile.TemporaryDirectory(prefix="logger-comparison-") as temp:
            directory = Path(temp)
            sample = dict(valid=True, queue_slot_bytes=1000,
                          queue_spill_capacity=0, queue_spill_storage_bytes=0,
                          producer_logs_per_sec=100, end_to_end_logs_per_sec=100,
                          enqueued=100, dropped=0, sync_fallbacks=0,
                          queue_spill_exhaustions=0)
            binaries = []
            for name, slot in (("memory", 4000), ("baseline", 1000), ("candidate", 1000)):
                binary = directory / name
                content = json.dumps(dict(sample, queue_slot_bytes=slot))
                binary.write_text("#!/usr/bin/env python3\nprint(" + repr(content) + ")\n")
                binary.chmod(0o755)
                binaries.append(str(binary))
            args = ["python3", str(ROOT / "scripts/compare_queue_benchmark.py"),
                    "--memory-baseline-bin", binaries[0],
                    "--throughput-baseline-bin", binaries[1],
                    "--candidate-bin", binaries[2], "--memory-baseline-ref", "fixture-memory",
                    "--throughput-baseline-ref", "fixture-throughput", "--threads", "1",
                    "--sizes", "64", "--records", "1", "--repeats", "1",
                    "--out-dir", str(directory / "results")]
            for mismatch in (False, True):
                proc = subprocess.run(args + (["--build-configs-differ"] if mismatch else []),
                                      text=True, capture_output=True, timeout=30)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                report = json.loads((directory / "results/results.json").read_text())
                self.assertTrue(report["memory"]["gate_passed"])
                self.assertEqual(report["benchmark"]["build_configs_differ"], mismatch)
                row = report["benchmark"]["rows"][0]
                self.assertEqual(row["throughput_comparable"], not mismatch)
                self.assertEqual(row["producer_ratio"], None if mismatch else 1.0)
                self.assertEqual(row["end_to_end_ratio"], None if mismatch else 1.0)
            proc = subprocess.run(args + ["--build-configs-differ", "--min-memory-reduction", "90"],
                                  text=True, capture_output=True, timeout=30)
            self.assertNotEqual(proc.returncode, 0)
            report = json.loads((directory / "results/results.json").read_text())
            self.assertFalse(report["memory"]["gate_passed"])

    def test_build_contract(self):
        evidence = Path(os.environ.get("BENCHMARK_BUILD_EVIDENCE",
                                       "benchmark-build-evidence")).resolve()
        evidence.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, LOGGER_PROJECT_VERSION=(ROOT / "VERSION").read_text().strip())
        with tempfile.TemporaryDirectory(prefix="logger-benchmark-") as temp:
            project = Path(temp) / "source"
            project.mkdir()
            # 复制当前跟踪文件内容，不修改调用方的 Xmake 配置或构建目录。
            files = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).split(b"\0")
            for name in filter(None, files):
                relative = Path(os.fsdecode(name))
                source = ROOT / relative
                if source.is_file():
                    destination = project / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)

            def run(label, args, success=True):
                proc = subprocess.run(args, cwd=project, env=env, text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      timeout=120)
                (evidence / (label + ".log")).write_text(proc.stdout)
                if success:
                    self.assertEqual(proc.returncode, 0, proc.stdout)
                else:
                    self.assertNotEqual(proc.returncode, 0, proc.stdout)
                return proc.stdout

            def configure(label, shared, regression, core=False):
                run(label, ["xmake", "f", "-y", "-c", "-m", "release", "-o", "build",
                            "--build_shared=" + shared,
                            "--build_tests=" + ("y" if core else "n"),
                            "--build_private_tests=n", "--build_regression_tests=" + regression])

            configure("static-config", "n", "n")
            log = run("static-build", ["xmake", "-rvD", "-j2", "bench_matrix", "bench_logger", "bench_fixed_rate"])
            verify_production(log, ["bench_matrix", "bench_logger", "bench_fixed_rate"])
            self.assertNotIn("logger_regression_support", log)
            self.assertNotIn("logger_test_support", log)
            archive = next((project / "build").rglob("liblogger.a"))
            run("static-artifact", ["python3", "scripts/check_production_artifact.py", str(archive)])
            binary = next(p for p in (project / "build").rglob("bench_matrix") if p.is_file())
            for size in (64, 1024, 4000):
                sample = json.loads(run(f"static-sample-{size}", [str(binary), "4", "1000", str(size)]))
                self.assertTrue(sample["valid"])
                self.assertEqual(sample["attempted"], 4000)
                self.assertEqual(sample["failed_records"], 0)
            dynamic = run("static-elf", ["readelf", "-d", str(binary)])
            self.assertNotIn("Shared library: [liblogger", dynamic)

            # 反例证明检查器拒绝支持库、缺少优化或错误诊断宏，而非无条件通过。
            for broken in (log.replace("-llogger ", "-llogger_regression_support "),
                           log.replace("-O3", "-O0"),
                           log.replace("-DLOGGER_ENABLE_LOCKDEP=0", "-DLOGGER_ENABLE_LOCKDEP=1"),
                           log.replace("-DLOGGER_ENABLE_FAULT_INJECTION=0", "-DLOGGER_ENABLE_FAULT_INJECTION=1")):
                with self.assertRaises(AssertionError):
                    verify_production(broken, ["bench_matrix", "bench_logger", "bench_fixed_rate"])

            for shared in ("n", "y"):
                mode = "shared" if shared == "y" else "static"
                # 清除旧可执行文件，避免静态产物掩盖共享配置的目标缺失。
                shutil.rmtree(project / "build")
                configure(mode + "-regression-config", shared, "y", core=True)
                targets = ["bench_logger", "bench_fixed_rate", "bench_matrix_accounting_regression"]
                if shared == "n":
                    targets.append("bench_matrix")
                log = run(mode + "-regression-build", ["xmake", "-rvD", "-j2", *targets])
                verify_production(log, ["bench_logger", "bench_fixed_rate"] + (["bench_matrix"] if shared == "n" else []))
                support = commands(log, "logger_regression_support", True)
                self.assertTrue(support)
                self.assertEqual(len(support), len(commands(log, "logger", True)))
                self.assertTrue(all("-DLOGGER_ENABLE_LOCKDEP=1" in c and
                                    "-DLOGGER_ENABLE_FAULT_INJECTION=0" in c and
                                    "-D_FORTIFY_SOURCE=0" in c and
                                    not any(a.startswith("-O") for a in c)
                                    for c in support))
                links = commands(log, "bench_matrix_accounting_regression", False)
                self.assertEqual(len(links), 1)
                self.assertIn("-llogger_regression_support", links[0])
                test = run(mode + "-regression-test", ["xmake", "test", "-j1",
                           "bench_matrix_accounting_regression/*"])
                self.assertIn("bench_matrix_accounting_regression/benchmark_accounting_smoke", test)
                self.assertRegex(ANSI.sub("", test), r"0 test\(s\) failed out of 1")
                run(mode + "-public-sample", ["xmake", "run", "bench_logger", "1", "100"])
                fixed = next(p for p in (project / "build").rglob("bench_fixed_rate") if p.is_file())
                fixed_library = next((project / "build").rglob(
                    "liblogger.a" if shared == "n" else "liblogger.so." + env["LOGGER_PROJECT_VERSION"]))
                fixed_log = evidence / (mode + "-regression-build.log")
                run(mode + "-fixed-rate-smoke", ["python3", "scripts/benchmark_fixed_rate.py",
                    "--binary", str(fixed), "--library", str(fixed_library),
                    "--build-log", str(fixed_log), "--out", str(evidence / (mode + "-fixed-rate")),
                    "--threads", "1", "--rates", "100", "--payload-bytes", "64",
                    "--seconds", "1", "--warmup", "0", "--repeats", "1"])
                run(mode + "-fixed-rate-budget-rejected", [str(fixed), "1", "2000000", "30", "5",
                    "3500", "/dev/null", str(project / "must-not-exist.raw")], success=False)
                self.assertFalse((project / "must-not-exist.raw").exists())
                if shared == "n":
                    test = run("static-production-test", ["xmake", "test", "-j1", "bench_matrix/*"])
                    self.assertIn("bench_matrix/production_benchmark_accounting_smoke", test)
                    self.assertRegex(ANSI.sub("", test), r"0 test\(s\) failed out of 1")
                else:
                    run("shared-private-rejected", ["xmake", "bench_matrix"], success=False)
                    self.assertFalse([p for p in (project / "build").rglob("bench_matrix") if p.is_file()])
                    binary = next(p for p in (project / "build").rglob("bench_logger") if p.is_file())
                    dynamic = run("shared-elf", ["readelf", "-d", str(binary)])
                    self.assertIn("Shared library: [liblogger.so.2]", dynamic)
                    library = next((project / "build").rglob("liblogger.so." + env["LOGGER_PROJECT_VERSION"]))
                    run("shared-artifact", ["python3", "scripts/check_production_artifact.py", str(library)])


if __name__ == "__main__":
    unittest.main()
