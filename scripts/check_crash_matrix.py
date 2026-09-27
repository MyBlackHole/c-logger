#!/usr/bin/env python3
"""Verify the exact Xmake process-crash suite and emit machine-readable evidence."""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


EXPECTED = {
    "crash_recovery_test/crash_after_audit_fsync",
    "crash_recovery_test/crash_before_state_rename",
    "crash_recovery_test/crash_after_state_rename",
    "crash_recovery_test/crash_after_checkpoint_commit",
    "audit_rotation_crash_regression/rotation_crash_sha256",
    "file_audit_regression/file_crash_sha256_file_after_archive_rename",
    "file_audit_regression/file_crash_sha256_file_after_archive_dirsync",
    "file_audit_regression/file_crash_sha256_file_after_active_open",
    "file_audit_regression/file_crash_sha256_file_after_active_dirsync",
}

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TEST_RESULT = re.compile(
    r"^\s*\[\s*\d+%\]:\s+(\S+/\S+)\s+.*?"
    r"\b(passed|failed)\s+([0-9.]+)s\s*$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify Xmake process-crash logs against the fixed nine-case contract."
    )
    parser.add_argument("logs", nargs="+", type=Path, help="xmake test output log")
    parser.add_argument(
        "--output",
        type=Path,
        help="optional JSON evidence file containing the observed test set per run",
    )
    parser.add_argument(
        "--junit-dir",
        type=Path,
        help="optional directory for one JUnit XML report per input log",
    )
    return parser.parse_args()


def results_from_log(path: Path) -> dict[str, tuple[str, float]]:
    results: dict[str, tuple[str, float]] = {}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = ANSI_ESCAPE.sub("", raw_line)
        match = TEST_RESULT.match(line)
        if match:
            results[match.group(1)] = (match.group(2), float(match.group(3)))
    return results


def write_junit(
    path: Path,
    log: Path,
    results: dict[str, tuple[str, float]],
    missing: set[str],
    unexpected: set[str],
) -> None:
    failures = len(missing) + len(unexpected)
    failures += sum(
        1 for name, (status, _) in results.items()
        if name in EXPECTED and status != "passed"
    )
    suite = ET.Element(
        "testsuite",
        {
            "name": "process-crash",
            "tests": str(len(EXPECTED) + len(unexpected)),
            "failures": str(failures),
            "errors": "0",
            "time": f"{sum(duration for _, duration in results.values()):.3f}",
        },
    )
    suite.set("source", str(log))

    for name in sorted(EXPECTED):
        status, duration = results.get(name, ("missing", 0.0))
        case = ET.SubElement(
            suite,
            "testcase",
            {"name": name, "classname": "process-crash", "time": f"{duration:.3f}"},
        )
        if status == "missing":
            ET.SubElement(case, "failure", {"message": "missing from Xmake test output"})
        elif status != "passed":
            ET.SubElement(case, "failure", {"message": f"Xmake status: {status}"})

    for name in sorted(unexpected):
        status, duration = results[name]
        case = ET.SubElement(
            suite,
            "testcase",
            {"name": name, "classname": "process-crash.unexpected", "time": f"{duration:.3f}"},
        )
        ET.SubElement(
            case,
            "failure",
            {"message": f"unexpected process-crash case with Xmake status: {status}"},
        )

    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def main() -> int:
    args = parse_args()
    evidence = {
        "expected": sorted(EXPECTED),
        "runs": [],
    }
    failed = False

    if args.junit_dir:
        args.junit_dir.mkdir(parents=True, exist_ok=True)

    for index, path in enumerate(args.logs, start=1):
        results = results_from_log(path)
        selected = set(results)
        missing = EXPECTED - selected
        unexpected = selected - EXPECTED
        failed_cases = sorted(
            name for name, (status, _) in results.items()
            if name in EXPECTED and status != "passed"
        )
        evidence["runs"].append(
            {
                "log": str(path),
                "tests": sorted(selected),
                "statuses": {
                    name: {"status": status, "seconds": duration}
                    for name, (status, duration) in sorted(results.items())
                },
                "missing": sorted(missing),
                "unexpected": sorted(unexpected),
                "failed": failed_cases,
            }
        )

        if args.junit_dir:
            write_junit(
                args.junit_dir / f"crash-junit-{index}.xml",
                path,
                results,
                missing,
                unexpected,
            )

        if missing or unexpected or failed_cases:
            failed = True
            print(
                f"{path}: crash suite mismatch/failure: "
                f"missing={sorted(missing)}, unexpected={sorted(unexpected)}, "
                f"failed={failed_cases}",
                file=sys.stderr,
            )
        else:
            print(
                f"{path}: verified {len(selected)} process-crash tests: "
                + ", ".join(sorted(selected))
            )

    if args.output:
        args.output.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
