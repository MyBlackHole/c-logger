#!/usr/bin/env python3
"""独立复核 TSan fork 跳过与同源 ASan 执行；不将跳过算成覆盖。"""
import argparse
import json
from pathlib import Path
import re

PUBLIC = ("async", "global", "nested", "repeated", "shutdown-first")
RUNTIME = ("advice-error", "bitmap-unmap-error", "directory-held",
           "earlier-handler", "emit-held", "format-held", "map-error",
           "mapped", "mutex-1", "mutex-2", "mutex-3", "mutex-4",
           "rwlock-error")
GROUPS = {
    "fork_reinit_public_test": PUBLIC,
    "fork_runtime_production_regression": RUNTIME,
    "fork_runtime_regression": RUNTIME,
}
EXPECTED_SKIPS = {f"{target}/{mode}" for target, modes in GROUPS.items()
                  for mode in modes}
TSAN_ACTIVE = {"fork_reinit_public_test/sync"}
TSAN_ACTIVE.update(f"{target}/{mode}-sync" for target in
                   ("fork_runtime_production_regression", "fork_runtime_regression")
                   for mode in ("mapped", "emit-held", "format-held", "directory-held"))
ANSI = re.compile(r"\x1b\[[0-9;]*m")
START = re.compile(r"^\[\s*\d+%\]: running\.test (?P<test>[^\s]+)$")
PASSED = re.compile(r"^\[\s*\d+%\]: (?P<test>[^\s]+)\s+.*?\bpassed\s+\d+(?:\.\d+)?s$")
SKIP = re.compile(r"^FORK_RUNTIME_SKIP=(?P<target>[^/\s]+)/(?P=target)/(?P<mode>[^/\s]+)$")
REASON = re.compile(r"^FORK_(?:REINIT|RUNTIME)_SKIP=tsan-post-fork-worker case=(?P<mode>[^\s]+)$")
SUMMARY = re.compile(r"^100% tests passed, 0 test\(s\) failed out of (?P<count>\d+),.*$")


def parse_log(content):
    """按 Xmake 的 running.test 分隔每个真实执行/明确跳过的输出块。"""
    lines = ANSI.sub("", content).splitlines()
    groups, passed, active = {}, set(), None
    wrapper_skips = []
    reasons = []
    summaries = []
    for line in lines:
        line = line.strip()
        start = START.fullmatch(line)
        if start:
            active = start.group("test")
            if active in groups:
                raise ValueError("duplicate_running_test:" + active)
            groups[active] = []
            continue
        if active is not None:
            groups[active].append(line)
        matched = PASSED.fullmatch(line)
        if matched:
            label = matched.group("test")
            if label in passed:
                raise ValueError("duplicate_passed_test:" + label)
            passed.add(label)
        skip = SKIP.fullmatch(line)
        if skip:
            wrapper_skips.append(f'{skip.group("target")}/{skip.group("mode")}')
        if line.startswith("FORK_RUNTIME_SKIP=") and not skip and not line.startswith(
                "FORK_RUNTIME_SKIP=tsan-post-fork-worker case="):
            raise ValueError("unknown_fork_skip_marker:" + line)
        reason = REASON.fullmatch(line)
        if reason:
            reasons.append(reason.group("mode"))
        if line.startswith("FORK_REINIT_SKIP=") and not reason:
            raise ValueError("unknown_reinit_skip_reason:" + line)
        summary = SUMMARY.fullmatch(line)
        if summary:
            summaries.append(int(summary.group("count")))
    if len(summaries) != 1 or len(groups) != summaries[0] or set(groups) != passed:
        raise ValueError("incomplete_full_suite_or_inconsistent_summary")
    if len(groups) < 500:
        raise ValueError("suite_not_full_static")
    return groups, wrapper_skips, reasons


def check_directory(directory, policy):
    if (directory / "test-exit-code.txt").read_text().strip() != "0":
        raise ValueError("suite_exit_not_zero:" + str(directory))
    actual_policy = (directory / "policies.txt").read_text().strip()
    if actual_policy != policy:
        raise ValueError("wrong_sanitizer_policy:" + actual_policy)
    source = (directory / "source-commit.txt").read_text().strip()
    tree = (directory / "source-tree.txt").read_text().strip()
    for ident in (source, tree):
        if not re.fullmatch(r"[0-9a-f]{40}", ident):
            raise ValueError("invalid_source_identity")
    log = (directory / "xmake-tests.log").read_text(errors="replace")
    groups, skips, reasons = parse_log(log)
    return {"source_commit": source, "source_tree": tree,
            "groups": groups, "skips": skips, "reasons": reasons}


def validate_model(directory):
    expected = {
        "healthy": (0, None),
        "order-error": (66, "WARNING: ThreadSanitizer: lock-order-inversion"),
        "race-error": (66, "WARNING: ThreadSanitizer: data race"),
    }
    rows = json.loads((directory / "results.json").read_text())
    if not isinstance(rows, list) or len(rows) != len(expected):
        raise ValueError("tsan_model_missing_controls")
    seen = set()
    for row in rows:
        mode = row["case"]
        if mode not in expected or mode in seen:
            raise ValueError("tsan_model_unexpected_or_duplicate_case")
        seen.add(mode)
        exitcode, warning = expected[mode]
        raw = (directory / (mode + ".log")).read_text(errors="replace")
        if (row["exit_code"] != exitcode or row["expected_exit"] != exitcode
                or row["accepted"] is not True or "parent had 1 threads" not in raw):
            raise ValueError("tsan_model_control_failed:" + mode)
        if warning is None:
            if "WARNING: ThreadSanitizer" in raw:
                raise ValueError("tsan_model_healthy_warned")
        elif warning not in raw:
            raise ValueError("tsan_model_did_not_detect_error:" + mode)
    return {"healthy": "executed_exit_0",
            "order-error": "real_diagnostic_exit_66",
            "race-error": "real_diagnostic_exit_66"}


def validate(tsan, asan, expected_source):
    if tsan["source_commit"] != expected_source or asan["source_commit"] != expected_source:
        raise ValueError("source_commit_mismatch")
    if tsan["source_tree"] != asan["source_tree"]:
        raise ValueError("source_tree_mismatch")
    if set(tsan["groups"]) != set(asan["groups"]):
        raise ValueError("suite_identity_mismatch")
    if asan["skips"] or asan["reasons"]:
        raise ValueError("asan_must_execute_no_fork_skips")
    if len(tsan["skips"]) != 31 or set(tsan["skips"]) != EXPECTED_SKIPS:
        raise ValueError("tsan_skip_count_or_set_mismatch")
    if len(tsan["reasons"]) != 31:
        raise ValueError("tsan_reason_count_mismatch")
    if len(tsan["groups"]) < 500:
        raise ValueError("tsan_incomplete")
    report = []
    for label in sorted(EXPECTED_SKIPS):
        target, mode = label.split("/", 1)
        tsan_lines = tsan["groups"][label]
        asan_lines = asan["groups"][label]
        tsan_wrapper = f"FORK_RUNTIME_SKIP={target}/{target}/{mode}"
        tsan_reason = (f"FORK_REINIT_SKIP=tsan-post-fork-worker case={mode}"
                       if target == "fork_reinit_public_test" else
                       f"FORK_RUNTIME_SKIP=tsan-post-fork-worker case={mode}")
        asan_pass = (f"FORK_REINIT_PUBLIC_PASS={mode}" if
                     target == "fork_reinit_public_test" else
                     f"FORK_RUNTIME_PASS={mode}")
        if tsan_lines.count(tsan_wrapper) != 1 or tsan_lines.count(tsan_reason) != 1:
            raise ValueError("tsan_skip_provenance_missing:" + label)
        if any("FORK_RUNTIME_PASS=" in line or "FORK_REINIT_PUBLIC_PASS=" in line
               for line in tsan_lines):
            raise ValueError("tsan_skipped_case_claims_execution:" + label)
        if not any(line.startswith(asan_pass) and
                   (len(line) == len(asan_pass) or line[len(asan_pass)] == " ")
                   for line in asan_lines):
            raise ValueError("asan_real_execution_marker_missing:" + label)
        report.append({"test": label, "tsan": "explicit_skip_post_fork_worker",
                       "asan_ubsan": "executed_pass"})
    for label in sorted(TSAN_ACTIVE):
        target, mode = label.split("/", 1)
        marker = (f"FORK_REINIT_PUBLIC_PASS={mode}" if
                  target == "fork_reinit_public_test" else f"FORK_RUNTIME_PASS={mode}")
        if not any(line.startswith(marker) for line in tsan["groups"][label]):
            raise ValueError("tsan_active_fork_anchor_missing:" + label)
    return {"source_commit": expected_source, "source_tree": tsan["source_tree"],
            "registered_tests": len(tsan["groups"]),
            "tsan_actual_pass": len(tsan["groups"]) - len(EXPECTED_SKIPS),
            "tsan_explicit_skip": len(EXPECTED_SKIPS),
            "asan_ubsan_actual_pass": len(asan["groups"]),
            "tsan_sync_anchor_pass": len(TSAN_ACTIVE),
            "skipped_cases": report,
            "limitation": "TSan unsupported post-fork worker is not dynamically covered"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsan-dir", required=True, type=Path)
    parser.add_argument("--asan-dir", required=True, type=Path)
    parser.add_argument("--model-dir", required=True, type=Path)
    parser.add_argument("--expected-source", required=True)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    try:
        tsan = check_directory(args.tsan_dir, "build.sanitizer.thread")
        asan = check_directory(args.asan_dir,
                               "build.sanitizer.address,build.sanitizer.undefined")
        result = validate(tsan, asan, args.expected_source)
        result["tsan_negative_controls"] = validate_model(args.model_dir)
    except (OSError, ValueError, KeyError) as err:
        (args.out / "failed.txt").write_text(str(err) + "\n")
        raise
    (args.out / "coverage.json").write_text(json.dumps(result, indent=2,
                                                       ensure_ascii=False) + "\n")
    print("FORK_TSAN_COVERAGE_AUDIT_OK"
          f" actual={result['tsan_actual_pass']} skip={result['tsan_explicit_skip']}"
          f" asan_actual={result['asan_ubsan_actual_pass']}")


if __name__ == "__main__":
    main()
