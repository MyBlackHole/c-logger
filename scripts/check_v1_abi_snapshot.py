#!/usr/bin/env python3
"""Verify the frozen Production v1 public ABI manifest."""

import argparse
import json
import re
from pathlib import Path


SYMBOL_RE = re.compile(r"^[a-z][a-z0-9_]+$")


def load_manifest(path: Path):
    symbols = []
    seen = set()
    errors = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        text = raw.strip()
        if not text or text.startswith("#"):
            continue
        if not SYMBOL_RE.fullmatch(text):
            errors.append({"path": str(path), "line": lineno, "invalid_symbol": text})
            continue
        if text in seen:
            errors.append({"path": str(path), "line": lineno, "duplicate_symbol": text})
            continue
        seen.add(text)
        symbols.append(text)
    return symbols, errors


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path,
                   default=root / "abi/logger-1.0.symbols")
    p.add_argument("--expected-count", type=int, default=62)
    a = p.parse_args()

    symbols, errors = load_manifest(a.manifest)
    if len(symbols) != a.expected_count:
        errors.append({
            "symbol_count": len(symbols),
            "expected_count": a.expected_count,
        })

    report = {
        "passed": not errors,
        "manifest": str(a.manifest),
        "symbol_count": len(symbols),
        "errors": errors,
    }
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
