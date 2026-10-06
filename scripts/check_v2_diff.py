#!/usr/bin/env python3
"""对 PR 新增源码行执行 2.0 基线规则，避免一次性重写 v1 遗留代码。"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

SOURCE_SUFFIXES = (".c", ".h", ".cc", ".cpp", ".hpp")

FORBIDDEN_CALLS = (
    ("strcpy", re.compile(r"\bstrcpy\s*\(")),
    ("strcat", re.compile(r"\bstrcat\s*\(")),
    ("sprintf", re.compile(r"\bsprintf\s*\(")),
)
STRNCPY = re.compile(r"\bstrncpy\s*\(")
RAW_ALWAYS_INLINE = re.compile(
    r"__attribute__\s*\(\(\s*always_inline\s*\)\)"
)


def source_diff(base: str) -> str:
    return subprocess.check_output(
        [
            "git",
            "diff",
            "--unified=0",
            "--no-color",
            f"{base}...HEAD",
            "--",
            "*.c",
            "*.h",
            "*.cc",
            "*.cpp",
            "*.hpp",
        ],
        text=True,
    )


def changed_source_paths(base: str) -> list[str]:
    out = subprocess.check_output(
        ["git", "diff", "--name-only", f"{base}...HEAD", "--"],
        text=True,
    )
    return [
        path
        for path in out.splitlines()
        if path.endswith(SOURCE_SUFFIXES)
    ]


def check_added_lines(base: str, errors: list[str]) -> None:
    path = ""

    for raw in source_diff(base).splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
            continue
        if not raw.startswith("+") or raw.startswith("+++"):
            continue

        line = raw[1:]
        if path == "src/logger_compiler.h":
            continue

        for name, pattern in FORBIDDEN_CALLS:
            if pattern.search(line):
                errors.append(f"{path}: new code uses forbidden {name}()")

        if STRNCPY.search(line) and "V2_FIXED_WIDTH_OK" not in line:
            errors.append(
                f"{path}: new strncpy() requires fixed-width justification "
                "and V2_FIXED_WIDTH_OK marker"
            )

        if RAW_ALWAYS_INLINE.search(line):
            errors.append(
                f"{path}: use logger_compiler.h abstraction "
                "instead of raw always_inline"
            )


def check_trailing_newline(base: str, errors: list[str]) -> None:
    for path in changed_source_paths(base):
        try:
            with open(path, "rb") as stream:
                data = stream.read()
        except FileNotFoundError:
            continue

        if data and not data.endswith(b"\n"):
            errors.append(f"{path}: source file must end with a newline")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("base", help="merge base ref, for example origin/main")
    args = parser.parse_args()

    errors: list[str] = []
    check_added_lines(args.base, errors)
    check_trailing_newline(args.base, errors)

    if errors:
        for item in errors:
            print(f"error: {item}", file=sys.stderr)
        return 1

    print("v2 added-line policy: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
