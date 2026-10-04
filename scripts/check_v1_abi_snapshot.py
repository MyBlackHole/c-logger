#!/usr/bin/env python3
"""Verify that the active public ABI manifest still matches the frozen v1 snapshot."""

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
    p.add_argument("--active", type=Path, default=root / "abi/logger.symbols")
    p.add_argument("--snapshot", type=Path, default=root / "abi/logger-1.0.symbols")
    a = p.parse_args()

    active, active_errors = load_manifest(a.active)
    frozen, frozen_errors = load_manifest(a.snapshot)
    errors = active_errors + frozen_errors

    active_set = set(active)
    frozen_set = set(frozen)
    if active_set != frozen_set:
        errors.append({
            "added_since_v1_snapshot": sorted(active_set - frozen_set),
            "missing_from_active": sorted(frozen_set - active_set),
        })
    elif active != frozen:
        errors.append("symbol order differs from frozen v1 snapshot")

    report = {
        "passed": not errors,
        "active": str(a.active),
        "snapshot": str(a.snapshot),
        "symbol_count": len(active),
        "errors": errors,
    }
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
