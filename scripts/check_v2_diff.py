#!/usr/bin/env python3
"""对 PR 新增源码行执行 2.0 基线规则，避免一次性重写 v1 遗留代码。"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

FORBIDDEN_CALLS = (
    ("strcpy", re.compile(r"\bstrcpy\s*\(")),
    ("strcat", re.compile(r"\bstrcat\s*\(")),
    ("sprintf", re.compile(r"\bsprintf\s*\(")),
)
STRNCPY = re.compile(r"\bstrncpy\s*\(")
RAW_ALWAYS_INLINE = re.compile(r"__attribute__\s*\(\(\s*always_inline\s*\)\)")


def diff(base: str) -> str:
    return subprocess.check_output(
        [
            "git", "diff", "--unified=0", "--no-color", f"{base}...HEAD", "--",
            "*.c", "*.h", "*.cc", "*.cpp", "*.hpp",
        ],
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("base", help="merge base ref, for example origin/main")
    args = parser.parse_args()

    path = ""
    errors: list[str] = []

    for raw in diff(args.base).splitlines():
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
                f"{path}: use logger_compiler.h abstraction instead of raw always_inline"
            )

    check_trailing_newline(args.base, errors)

    if errors:
        for item in errors:
            print(f"error: {item}", file=sys.stderr)
        return 1

    print("v2 added-line policy: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
