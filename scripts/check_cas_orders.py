#!/usr/bin/env python3
"""Review gate for explicit C11 CAS orders in src/ and include/.

This is a lexical inventory, not a C preprocessor or a happens-before proof.
All source branches are checked. Direct standard CAS calls must use literal
memory_order_* arguments; aliases, implicit orders and compiler CAS builtins
need an explicit checker/inventory review rather than silently escaping it.
"""
from __future__ import annotations

import argparse
from collections import Counter
import csv
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FIELDS = ("file", "operation", "object", "desired", "success", "failure",
          "count", "rationale")
# Conservative C11 contract (N1570 7.17.7.4p2). In particular, do not rely on
# the historical release/acquire "no stronger" ambiguity or C++ rule changes.
ALLOWED_FAILURES = {
    "relaxed": {"relaxed"},
    "consume": {"relaxed", "consume"},
    "acquire": {"relaxed", "consume", "acquire"},
    "release": {"relaxed"},
    "acq_rel": {"relaxed", "consume", "acquire"},
    "seq_cst": {"relaxed", "consume", "acquire", "seq_cst"},
}
NON_CODE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                      re.DOTALL)
CAS_NAME = re.compile(r"\b(?:\w*compare_exchange\w*|"
                      r"__sync_(?:bool|val)_compare_and_swap)\b")
EXPLICIT = {"atomic_compare_exchange_strong_explicit": "strong",
            "atomic_compare_exchange_weak_explicit": "weak"}


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


def arguments(source: str, start: int) -> list[str]:
    """Split a call without confusing nested desired/object expressions."""
    match = re.match(r"\s*\(", source[start:])
    if not match:
        raise ValueError("CAS must be a direct call, not an alias")
    begin = start + match.end()
    stack = [")"]
    args = []
    pairs = {"(": ")", "[": "]", "{": "}"}
    for pos in range(begin, len(source)):
        char = source[pos]
        if char in pairs:
            stack.append(pairs[char])
        elif char in ")]}":
            if char != stack.pop():
                raise ValueError("unbalanced CAS arguments")
            if not stack:
                args.append(compact(source[begin:pos]))
                return args
        elif char == "," and len(stack) == 1:
            args.append(compact(source[begin:pos]))
            begin = pos + 1
    raise ValueError("unterminated CAS call")


def scan(root: Path) -> tuple[Counter, list[str]]:
    observed = Counter()
    errors = []
    for directory in ("src", "include"):
        base = root / directory
        if not base.is_dir():
            errors.append(f"missing source directory: {directory}")
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or not (path.suffix in (".c", ".h") or
                                          path.name.endswith(".h.in")):
                continue
            relative = path.relative_to(root).as_posix()
            source = path.read_text(encoding="utf-8")
            source = NON_CODE.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), source)
            for match in CAS_NAME.finditer(source):
                location = f"{relative}:{source.count(chr(10), 0, match.start()) + 1}"
                operation = EXPLICIT.get(match.group())
                if operation is None:
                    errors.append(f"{location}: unreviewed CAS API {match.group()}")
                    continue
                try:
                    args = arguments(source, match.end())
                    if len(args) != 5 or not all(args):
                        raise ValueError("expected five explicit CAS arguments")
                    orders = []
                    for arg in args[-2:]:
                        if not arg.startswith("memory_order_"):
                            raise ValueError("CAS orders must be literal memory_order_* names")
                        orders.append(arg[len("memory_order_"):])
                    success, failure = orders
                    if failure not in ALLOWED_FAILURES.get(success, set()):
                        raise ValueError(f"unapproved C11 order pair {success}/{failure}")
                    observed[(relative, operation, args[0], args[2], success, failure)] += 1
                except ValueError as error:
                    errors.append(f"{location}: {error}")
    return observed, errors


def inventory(path: Path) -> Counter:
    expected = Counter()
    with path.open(encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        if tuple(rows.fieldnames or ()) != FIELDS:
            raise ValueError("invalid CAS inventory header")
        for line, row in enumerate(rows, 2):
            if set(row) != set(FIELDS) or not all(row.values()):
                raise ValueError(f"inventory line {line}: missing/extra field")
            key = tuple(row[field] for field in FIELDS[:6])
            if key in expected:
                raise ValueError(f"inventory line {line}: duplicate entry")
            if row["operation"] not in ("strong", "weak") or row["failure"] not in \
                    ALLOWED_FAILURES.get(row["success"], set()):
                raise ValueError(f"inventory line {line}: unapproved CAS operation/order pair")
            count = int(row["count"])
            if count < 1 or not row["rationale"].strip():
                raise ValueError(f"inventory line {line}: missing count/rationale")
            expected[key] = count
    if not expected:
        raise ValueError("empty CAS inventory")
    return expected


def check(root: Path) -> tuple[int, list[str]]:
    observed, errors = scan(root)
    expected = inventory(root / "validation/cas_orders.tsv")
    for key, count in (observed - expected).items():
        errors.append(f"unreviewed/changed CAS ({count}): {' | '.join(key)}")
    for key, count in (expected - observed).items():
        errors.append(f"missing/changed CAS ({count}): {' | '.join(key)}")
    return sum(observed.values()), errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        count, errors = check(args.root)
    except (OSError, ValueError, csv.Error) as error:
        print(f"CAS order gate: {error}", file=sys.stderr)
        return 1
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"CAS order gate: {count} reviewed calls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
