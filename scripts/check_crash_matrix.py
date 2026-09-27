#!/usr/bin/env python3
"""Fail if the focused Xmake process-crash suite silently loses or gains cases."""

import argparse
import json
import re
import sys
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
    r"^\s*\[\s*\d+%\]:\s+(\S+/\S+)\s+.*\b(?:passed|failed)\b"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the exact Xmake process-crash test set in one or more logs."
    )
    parser.add_argument("logs", nargs="+", type=Path, help="xmake test output log")
    parser.add_argument(
        "--output",
        type=Path,
        help="optional JSON evidence file containing the observed test set per run",
    )
    return parser.parse_args()


def tests_from_log(path: Path) -> set[str]:
    selected: set[str] = set()
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = ANSI_ESCAPE.sub("", raw_line)
        match = TEST_RESULT.match(line)
        if match:
            selected.add(match.group(1))
    return selected


def main() -> int:
    args = parse_args()
    evidence = {
        "expected": sorted(EXPECTED),
        "runs": [],
    }
    failed = False

    for path in args.logs:
        selected = tests_from_log(path)
        missing = EXPECTED - selected
        unexpected = selected - EXPECTED
        evidence["runs"].append(
            {
                "log": str(path),
                "tests": sorted(selected),
                "missing": sorted(missing),
                "unexpected": sorted(unexpected),
            }
        )

        if missing or unexpected:
            failed = True
            print(
                f"{path}: crash suite mismatch: "
                f"missing={sorted(missing)}, unexpected={sorted(unexpected)}",
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
