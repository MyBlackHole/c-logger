#!/usr/bin/env python3
"""#211: restore immutable evidence and replay it with a fresh, pinned-source build.

The old binaries were not archived. A successful replay is a new experiment,
not recovery of the old executable or reproduction of its coverage counters.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
TARGETS = {
    "public-fuzzer": 11896199906,
    "format-fuzzer": 11928575927,
    "format-plain": 11928595589,
    "format-address-undefined": 11928516389,
    "format-thread": 11927974212,
}
FORMAT_RESULT = re.compile(r"format PASS seed=(\d+) inputs=512 rejected=(0|3072)$")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def metadata(root, mode):
    doc = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    require(doc["schema_version"] == 1, "unsupported_manifest_schema")
    entries = [e for e in doc["artifacts"] if e["artifact_id"] == TARGETS[mode]]
    require(len(entries) == 1, "missing_or_duplicate_artifact")
    entry = entries[0]
    require(re.fullmatch(r"[0-9a-f]{40}", entry["tested_source_sha"]),
            "invalid_tested_source_sha")
    return entry


def toolchain_supplement(root):
    doc = json.loads((root / "toolchain-supplement.json").read_text(encoding="utf-8"))
    require(doc["artifact_id"] == TARGETS["public-fuzzer"], "wrong_supplement_artifact")
    require(doc["job_id"] == 115866817217, "wrong_supplement_job")
    require(doc["excerpt_file"] == "pr208-toolchain-excerpt.txt", "wrong_excerpt_path")
    excerpt = (root / doc["excerpt_file"]).read_bytes()
    require(sha(excerpt) == doc["excerpt_sha256"], "toolchain_excerpt_digest_mismatch")
    for key in ("compiler_line", "xmake_observed_banner"):
        require(doc[key] in excerpt.decode("utf-8"), "toolchain_line_not_in_excerpt")
    require(doc["tested_source_sha"] == metadata(root, "public-fuzzer")["tested_source_sha"],
            "toolchain_supplement_source_mismatch")
    return doc


def snapshot(directory):
    require(directory.is_dir() and not directory.is_symlink(), "missing_restore")
    result = {}
    for path in sorted(directory.rglob("*")):
        require(not path.is_symlink(), "symlink_in_restored_evidence")
        if path.is_file():
            result[path.relative_to(directory).as_posix()] = sha(path.read_bytes())
    require(result, "empty_restore")
    return result


def restored_expected(originals, decoded):
    result = {name: sha(data) for name, data in originals.items()}
    for artifact_id, members in decoded.items():
        result.update({f"{artifact_id}/{name}": sha(data)
                       for name, data in members.items()})
    return result


def corpus_files(directory):
    require(directory.is_dir() and not directory.is_symlink(), "missing_corpus")
    files = sorted(directory.rglob("*"))
    require(files and len(files) <= 1024, "empty_or_oversized_corpus")
    require(all(p.is_file() and not p.is_symlink() for p in files),
            "nonregular_corpus_member")
    return files


def format_commands(text):
    lines = text.strip().splitlines()
    results = [FORMAT_RESULT.fullmatch(line) for line in lines]
    require(len(results) == 5 and all(results), "invalid_recorded_property_log")
    pairs = [(m.group(1), m.group(2)) for m in results]
    require(pairs == [("1", "0"), ("107", "0"), ("20261015", "0"),
                      ("4294967295", "0"), ("107", "3072")],
            "unexpected_recorded_property_seeds")
    return list(zip(["1", "107", "20261015", "4294967295", "selftest"], lines))


def run(command, cwd, logfile, records, timeout=180):
    # Never invoke a shell for restored input paths. stdout/stderr are retained
    # even on failure/timeout, and every attempted command enters the receipt.
    record = {"argv": [str(x) for x in command], "cwd": str(cwd),
              "log": logfile.name}
    records.append(record)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
               TSAN_OPTIONS="halt_on_error=1")
    try:
        with logfile.open("xb") as output:
            process = subprocess.run(record["argv"], cwd=cwd, env=env,
                                     stdout=output, stderr=subprocess.STDOUT,
                                     timeout=timeout, check=False)
        record["returncode"] = process.returncode
    except subprocess.TimeoutExpired:
        record["status"] = "timeout"
        raise ValueError("replay_command_timeout")
    finally:
        if logfile.is_file():
            record["log_sha256"] = sha(logfile.read_bytes())
    require(process.returncode == 0, "replay_command_failed: " + logfile.name)
    record["status"] = "pass"
    return logfile.read_text(encoding="utf-8", errors="replace")


def git_value(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args],
                                   text=True, timeout=30).strip()


def experiment(args):
    root = HERE
    entry = metadata(root, args.mode)
    source = args.source_dir.resolve()
    require(git_value(source, "rev-parse", "HEAD") == entry["tested_source_sha"],
            "source_does_not_match_archived_test_checkout")
    require(not git_value(source, "status", "--porcelain", "--untracked-files=all"),
            "source_checkout_not_clean")
    output = args.output_dir.resolve()
    require(not output.is_relative_to(source), "output_must_be_outside_source")
    output.mkdir(parents=True, exist_ok=False)
    receipt = {
        "schema_version": 1, "status": "running", "mode": args.mode,
        "scope": "new_build_replay_not_original_binary_or_original_run",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "archive_artifact": entry,
        "original_toolchain_supplement": toolchain_supplement(root),
        "replay_source_sha": git_value(source, "rev-parse", "HEAD"),
        "replay_source_tree": git_value(source, "rev-parse", "HEAD^{tree}"),
        "verification_tool_sha": git_value(root, "rev-parse", "HEAD"),
        "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "github_job": os.environ.get("GITHUB_JOB"),
        "runner_image": os.environ.get("ImageOS"),
        "runner_image_version": os.environ.get("ImageVersion"),
        "manifest_sha256": sha((root / "manifest.json").read_bytes()),
        "commands": [],
    }
    records = receipt["commands"]

    def execute(command, name, timeout=180):
        return run(command, source, output / name, records, timeout)

    try:
        spec = importlib.util.spec_from_file_location("archive_verifier", root / "verify.py")
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        originals, decoded, results = verifier.verify(root)
        restored = output / "restored"
        verifier.restore(restored, originals, decoded)
        expected = restored_expected(originals, decoded)
        require(snapshot(restored) == expected, "restore_bytes_differ")
        receipt["restore"] = {"artifacts": results,
                              "members": sum(map(len, decoded.values())),
                              "all_restored_files_sha256": expected}
        receipt["historical_binary_hash_record"] = decoded[str(entry["artifact_id"])][
            entry["binary_hash_record"]].decode("utf-8")
        # Compiler/library builds use historical source only, not files from ZIPs.
        fuzz = args.mode.endswith("fuzzer")
        compiler = "clang" if fuzz or args.mode.endswith("address-undefined") else "gcc"
        receipt["compiler"] = execute([compiler, "--version"], "compiler.log")
        receipt["xmake"] = execute(["xmake", "--version"], "xmake.log")
        build = output / "build"
        os.environ["LOGGER_PROJECT_VERSION"] = (source / "VERSION").read_text().strip()
        config = ["xmake", "f", "-m", "debug", "-o", str(build), "--build_shared=n",
                  "--build_tests=n", "--build_private_tests=n", "--build_regression_tests=n",
                  "--toolchain=" + compiler]
        sanitizers = []
        if fuzz or args.mode.endswith("address-undefined"):
            config += ["--policies=build.sanitizer.address,build.sanitizer.undefined"]
            sanitizers = ["-fsanitize=" + ("fuzzer," if fuzz else "") + "address,undefined"]
        elif args.mode.endswith("thread"):
            config += ["--policies=build.sanitizer.thread"]
            sanitizers = ["-fsanitize=thread"]
        if fuzz:
            config += ["--cxflags=-fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp"]
        execute(config, "configure.log")
        execute(["xmake", "-rvD", "-j2", "logger"], "build.log", 300)
        libraries = list(build.rglob("liblogger.a"))
        require(len(libraries) == 1, "not_one_production_target_archive")
        library = libraries[0]
        if fuzz:
            symbols = execute(["nm", "-u", str(library)], "coverage-symbols.log")
            for symbol in ("__sanitizer_cov_8bit_counters_init", "__sanitizer_cov_pcs_init"):
                require(symbol in symbols, "missing_library_coverage: " + symbol)
        driver = output / "replay-driver"
        harness = "public_fuzz.c" if args.mode.startswith("public") else "format_fuzz.c"
        command = [compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic",
                   "-Werror", "-DLOGGER_STATIC_DEFINE=1", "-Iinclude", "-Isrc",
                   "-I" + str(build / "generated"), "-fno-omit-frame-pointer", *sanitizers]
        if not fuzz:
            command += ["-DLOGGER_FUZZ_STANDALONE=1"]
        command += ["tests/fuzz/" + harness, str(library), "-pthread", "-o", str(driver)]
        execute(command, "driver-build.log")
        receipt["new_library_sha256"] = sha(library.read_bytes())
        receipt["new_driver_sha256"] = sha(driver.read_bytes())
        artifact_dir = restored / str(entry["artifact_id"])
        if fuzz:
            inputs = corpus_files(artifact_dir / "corpus")
            receipt["replayed_inputs"] = []
            for index, path in enumerate(inputs):
                text = execute([str(driver), "-runs=1", "-timeout=10", str(path)],
                               f"input-{index:04}.log", 30)
                require("Executed " in text and "Running: " in text,
                        "input_not_executed_by_libfuzzer")
                receipt["replayed_inputs"].append({
                    "path": path.relative_to(restored).as_posix(),
                    "sha256": sha(path.read_bytes()), "status": "pass"})
        else:
            recorded = (artifact_dir / entry["result_record"]).read_text()
            receipt["replayed_seeds"] = []
            for seed, expected_line in format_commands(recorded):
                text = execute([str(driver), seed], "seed-" + seed + ".log", 60)
                require(text.strip() == expected_line, "property_result_differs_from_record")
                receipt["replayed_seeds"].append({"argument": seed, "result": text.strip()})
        require(snapshot(restored) == expected, "restored_evidence_changed_during_replay")
        require(not git_value(source, "status", "--porcelain", "--untracked-files=all"),
                "historical_source_changed_during_replay")
        receipt["status"] = "pass"
    except Exception as error:
        receipt["status"] = "fail"
        receipt["error"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "receipt.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2)
                                            + "\n", encoding="utf-8")
    print(json.dumps({"status": "pass", "mode": args.mode,
                      "restored_members": receipt["restore"]["members"],
                      "replayed": len(receipt.get("replayed_inputs", receipt.get("replayed_seeds", [])))},
                     sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=TARGETS, required=True)
    parser.add_argument("--print-source", action="store_true")
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.print_source:
        print(metadata(HERE, args.mode)["tested_source_sha"])
        return 0
    if args.source_dir is None or args.output_dir is None:
        parser.error("--source-dir and --output-dir are required for replay")
    try:
        experiment(args)
        return 0
    except Exception as error:
        print(f"archive replay failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
