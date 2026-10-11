#!/usr/bin/env python3
"""核验 Xmake 进程崩溃测试的精确集合，并输出机器可读证据。"""

from __future__ import annotations

# JSON/JUnit 证据保持原样；仅人读帮助与终端汇总进入翻译目录。
import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from crash_contract import MIGRATION, POINTS


EXPECTED = {f"crash_recovery_test/crash_{point}" for point in POINTS}

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TEST_RESULT = re.compile(
    r"^\s*\[\s*\d+%\]:\s+(\S+/\S+)\s+.*?"
    r"\b(passed|failed)\s+([0-9.]+)s\s*$"
)


def parse_args() -> argparse.Namespace:
    from tool_i18n import parser_for
    parser, _ = parser_for('Verify Xmake process-crash logs against the fixed eight-case ordinary Logger contract.')
    parser.add_argument("logs", nargs="+", type=Path, help=_('xmake test output log'))
    parser.add_argument(
        "--output",
        type=Path,
        help=_('optional JSON evidence file containing the observed test set per run'),
    )
    parser.add_argument(
        "--junit-dir",
        type=Path,
        help=_('optional directory for one JUnit XML report per input log'),
    )
    return parser.parse_args()


def results_from_log(path: Path) -> dict[str, tuple[str, float]]:
    results: dict[str, tuple[str, float]] = {}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = ANSI_ESCAPE.sub("", raw_line)
        match = TEST_RESULT.match(line)
        if match:
            name, status, seconds = match.groups()
            if name in results:
                # 重复结果不得覆盖先前的失败，也不得让意外重复执行的测试
                # 看起来仍符合精确集合。
                results[name] = ("duplicate", results[name][1] + float(seconds))
            else:
                results[name] = (status, float(seconds))
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
    from tool_i18n import resolve_languages, translation
    args = parse_args()
    _ = translation(resolve_languages(args.lang)).gettext
    evidence = {
        "contract": "ordinary-logger-crash-v1",
        "boundary": "process SIGKILL; not VM or physical power loss",
        "migration": MIGRATION,
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
                _("{path}: crash suite mismatch/failure: missing={missing}, unexpected={unexpected}, failed={failed}").format(
                    path=path, missing=sorted(missing), unexpected=sorted(unexpected), failed=failed_cases),
                file=sys.stderr,
            )
        else:
            print(
                _("{path}: verified {count} process-crash tests: {tests}").format(
                    path=path, count=len(selected), tests=", ".join(sorted(selected)))
            )

    if args.output:
        args.output.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
