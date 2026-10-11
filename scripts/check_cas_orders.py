#!/usr/bin/env python3
"""检查 src/ 和 include/ 中显式 C11 CAS 的内存序与评审清单。

这是词法清单，不是 C 预处理器，也不是先行发生关系证明。
检查源码中的所有分支。标准 CAS 直接调用必须使用 memory_order_* 字面量；
别名、隐式内存序和编译器 CAS 内建函数必须先调整检查器与评审清单。
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
# 采用保守、无歧义的 C11 约定（N1570 7.17.7.4p2），不依赖历史上
# release/acquire 的“失败序不得更强”歧义，也不套用 C++ 后续规则。
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
    """拆分调用参数，保留嵌套的 desired/object 表达式。"""
    match = re.match(r"\s*\(", source[start:])
    if not match:
        raise ValueError("CAS 必须直接调用，不得使用未评审的别名")
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
                raise ValueError("CAS 参数括号不匹配")
            if not stack:
                args.append(compact(source[begin:pos]))
                return args
        elif char == "," and len(stack) == 1:
            args.append(compact(source[begin:pos]))
            begin = pos + 1
    raise ValueError("CAS 调用未闭合")


def scan(root: Path) -> tuple[Counter, list[str]]:
    observed = Counter()
    errors = []
    for directory in ("src", "include"):
        base = root / directory
        if not base.is_dir():
            errors.append(f"缺少源码目录： {directory}")
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
                    errors.append(f"{location}: 未经评审的 CAS API {match.group()}")
                    continue
                try:
                    args = arguments(source, match.end())
                    if len(args) != 5 or not all(args):
                        raise ValueError("显式 CAS 调用必须具有五个参数")
                    orders = []
                    for arg in args[-2:]:
                        if not arg.startswith("memory_order_"):
                            raise ValueError("CAS 内存序必须使用 memory_order_* 字面量名称")
                        orders.append(arg[len("memory_order_"):])
                    success, failure = orders
                    if failure not in ALLOWED_FAILURES.get(success, set()):
                        raise ValueError(f"未获准的 C11 内存序组合 {success}/{failure}")
                    observed[(relative, operation, args[0], args[2], success, failure)] += 1
                except ValueError as error:
                    errors.append(f"{location}: {error}")
    return observed, errors


def inventory(path: Path) -> Counter:
    expected = Counter()
    with path.open(encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        if tuple(rows.fieldnames or ()) != FIELDS:
            raise ValueError("CAS 清单表头无效")
        for line, row in enumerate(rows, 2):
            if set(row) != set(FIELDS) or not all(row.values()):
                raise ValueError(f"清单行 {line}: 字段缺失或多余")
            key = tuple(row[field] for field in FIELDS[:6])
            if key in expected:
                raise ValueError(f"清单行 {line}: 重复条目")
            if row["operation"] not in ("strong", "weak") or row["failure"] not in \
                    ALLOWED_FAILURES.get(row["success"], set()):
                raise ValueError(f"清单行 {line}: 未获准的 CAS 操作或内存序组合")
            count = int(row["count"])
            if count < 1 or not row["rationale"].strip():
                raise ValueError(f"清单行 {line}: 缺少有效计数或理由")
            expected[key] = count
    if not expected:
        raise ValueError("CAS 清单为空")
    return expected


def check(root: Path) -> tuple[int, list[str]]:
    observed, errors = scan(root)
    expected = inventory(root / "validation/cas_orders.tsv")
    for key, count in (observed - expected).items():
        errors.append(f"未经评审或已变更的 CAS ({count}): {' | '.join(key)}")
    for key, count in (expected - observed).items():
        errors.append(f"缺失或已变更的 CAS ({count}): {' | '.join(key)}")
    return sum(observed.values()), errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        count, errors = check(args.root)
    except (OSError, ValueError, csv.Error) as error:
        print(f"CAS 内存序门禁： {error}", file=sys.stderr)
        return 1
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"CAS 内存序门禁： {count} 处已评审调用通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
