#!/usr/bin/env python3
"""Fail if the focused CI crash suite silently loses or gains test cases."""

import json
import sys
from pathlib import Path


EXPECTED = {
    "crash_after_audit_fsync",
    "crash_before_state_rename",
    "crash_after_state_rename",
    "crash_after_checkpoint_commit",
    "rotation_crash_sha256",
    "file_crash_sha256_file_after_archive_rename",
    "file_crash_sha256_file_after_archive_dirsync",
    "file_crash_sha256_file_after_active_open",
    "file_crash_sha256_file_after_active_dirsync",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_crash_matrix.py <ctest-json>", file=sys.stderr)
        return 2
    tests = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["tests"]
    selected = {
        test["name"]
        for test in tests
        if "crash" in next(
            (prop["value"] for prop in test.get("properties", []) if prop["name"] == "LABELS"),
            [],
        )
    }
    missing, unexpected = EXPECTED - selected, selected - EXPECTED
    if missing or unexpected:
        print(f"crash suite mismatch: missing={sorted(missing)}, unexpected={sorted(unexpected)}", file=sys.stderr)
        return 1
    print(f"Verified {len(selected)} crash recovery tests: {', '.join(sorted(selected))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
