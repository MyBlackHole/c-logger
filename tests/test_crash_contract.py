#!/usr/bin/env python3
"""Mutation checks for the ordinary Logger crash oracle (no crash is simulated)."""

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from crash_contract import POINTS  # noqa: E402
from check_crash_matrix import EXPECTED  # noqa: E402


def line(sequence):
    return (
        "2023-11-14T22:13:20.123456+0000 INFO  "
        f"LOGGER_CRASH_V1 seq={sequence:03d} payload="
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "/full-content-must-survive/0123456789abcdefghijklmnopqrstuvwxyz"
        f" end={sequence:03d}\n"
    ).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    tested = 0

    def check(label, data, point="acknowledged", recovered=0, valid=False,
              archives=(), extra=None):
        nonlocal tested
        with tempfile.TemporaryDirectory(prefix="logger-oracle-") as name:
            root = Path(name)
            if data is not None:
                (root / "crash.log").write_bytes(data)
            for i, content in enumerate(archives):
                (root / f"crash.20231114T221320.{123456 + i:06d}Z.log").write_bytes(content)
            if extra:
                extra(root)
            result = subprocess.run(
                [str(binary), "--verify", point, str(recovered)], cwd=root,
                capture_output=True, timeout=5,
            )
            if (result.returncode == 0) != valid:
                raise AssertionError(f"{label}: rc={result.returncode}\n"
                                     f"{result.stdout!r}\n{result.stderr!r}")
            tested += 1

    baseline = b"".join(line(i) for i in range(10))
    recoveries = line(11) + line(12)
    target = line(10)
    check("baseline", baseline, valid=True)
    check("full post-fsync target", baseline + target,
          point="after_file_fsync", valid=True)
    check("missing post-fsync target", baseline, point="after_file_fsync")
    check("unexpected acknowledged target", baseline + target)
    check("wrong cut point", baseline, point="after_checkpoint_commit")
    for i in range(10):
        check(f"missing baseline {i}", baseline.replace(line(i), b""))
        check(f"duplicate baseline {i}", baseline + line(i))
        check(f"payload mutation {i}", baseline.replace(
            line(i), line(i).replace(b"full-content", b"BAD!-content")))
    for prefix in range(len(target) + 1):
        check(f"permitted target prefix {prefix}", baseline + target[:prefix],
              point="before_file_fsync", valid=True)
        check(f"prefix {prefix} then complete recovery suffix",
              baseline + target[:prefix] + recoveries,
              point="before_file_fsync", recovered=2, valid=True)
    for tail in (b"garbage", target + target, line(0), target[:-1] + b"X"):
        check("invalid unconfirmed tail", baseline + tail,
              point="before_file_fsync")
    check("missing recovery", baseline + line(11), recovered=2)
    check("duplicate recovery", baseline + line(11) * 2, recovered=2)
    check("reordered recovery", baseline + line(12) + line(11), recovered=2)
    check("corrupt recovery", baseline + recoveries[:-1] + b"X", recovered=2)
    archives = tuple(line(i) for i in range(10))
    check("archive rename committed, active absent", None,
          point="file_after_archive_rename", archives=archives, valid=True)
    check("archive rename rolled back", line(9),
          point="file_after_archive_rename", archives=archives[:9], valid=True)
    check("duplicate baseline across archive and active", line(9),
          point="file_after_archive_rename", archives=archives)
    check("duplicate baseline across archives", None,
          point="file_after_archive_rename", archives=archives[:9] + (line(8),))
    check("rotation recovery suffix", recoveries,
          point="file_after_active_dirsync", archives=archives, recovered=2, valid=True)
    check("rotated post-fsync target", target,
          point="rotation_after_file_fsync", archives=archives, valid=True)
    check("partial archived baseline", None, point="file_after_archive_rename",
          archives=archives[:9] + (line(9)[:-1],))
    check("unexpected namespace entry", baseline,
          extra=lambda root: (root / "crash.unexpected.log").write_bytes(b""))
    check("hard-linked active", baseline,
          extra=lambda root: (root / "alias").hardlink_to(root / "crash.log"))
    def symlink(root):
        (root / "crash.log").rename(root / "real")
        (root / "crash.log").symlink_to("real")
    check("symlink active", baseline, extra=symlink)

    # All harness selectors must register exactly the reviewed eight names.
    xmake = (ROOT / "xmake.lua").read_text()
    section = xmake.split('target("crash_recovery_test")', 1)[1].split('target_end()', 1)[0]
    declared = set(re.findall(r'"((?:acknowledged|before_file_fsync|after_file_fsync|rotation_after_file_fsync|file_after_[a-z_]+))"', section))
    assert declared == set(POINTS), (declared, POINTS)
    assert EXPECTED == {f"crash_recovery_test/crash_{point}" for point in POINTS}
    workflow = (ROOT / ".github/workflows/vm-powercut.yml").read_text()
    matrix = workflow.split("      matrix:", 1)[1].split("    steps:", 1)[0]
    assert "filesystem: [ext4, xfs]" in matrix
    declared = set(re.findall(r"^          - ([a-z_]+)$", matrix, re.MULTILINE))
    assert declared == set(POINTS), (declared, POINTS)
    result_lines = [f"[100%]: {name} ........ passed 0.001s" for name in sorted(EXPECTED)]
    with tempfile.TemporaryDirectory(prefix="logger-matrix-") as name:
        log = Path(name) / "xmake.log"
        cases = {
            "complete": (result_lines, True),
            "missing": (result_lines[:-1], False),
            "duplicate": (result_lines + result_lines[:1], False),
            "failed": ([result_lines[0].replace("passed", "failed")] + result_lines[1:], False),
            "unexpected": (result_lines + ["[100%]: other/unexpected ... passed 0.001s"], False),
            "empty": ([], False),
        }
        for label, (rows, valid) in cases.items():
            log.write_text("\n".join(rows) + "\n")
            result = subprocess.run([sys.executable, str(ROOT / "scripts/check_crash_matrix.py"),
                                     str(log)], capture_output=True, timeout=5)
            assert (result.returncode == 0) == valid, (label, result.stdout, result.stderr)
    print(f"crash verifier: {tested} fixture acceptance/rejection checks passed; "
          "six log-matrix checks passed; process/VM matrices match the eight-point contract")


if __name__ == "__main__":
    main()
