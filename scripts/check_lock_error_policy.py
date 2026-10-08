#!/usr/bin/env python3
"""检查可失败互斥锁路径是否显式处理获取与释放结果。"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def fail(message: str, errors: list[str]) -> None:
    errors.append(message)


def check_guard_definitions(errors: list[str]) -> None:
    header = (SRC / "logger_cleanup.h").read_text(encoding="utf-8")
    if re.search(
        r"(?m)^(?!\s*#define\s)\s*DEFINE_GUARD\s*\(\s*pthread_mutex\s*,",
        header,
    ):
        fail("logger_cleanup.h 仍生成忽略 pthread 获取结果的基础 guard", errors)
    macro_start = header.find("#define DEFINE_LOCKDEP_MUTEX_GUARD")
    macro_end = header.find("\nDEFINE_LOCKDEP_MUTEX_GUARD(", macro_start)
    macro = header[macro_start : macro_end if macro_end >= 0 else None]
    if re.search(r"\bDEFINE_GUARD\s*\(", macro):
        fail("实例锁 guard 宏重新引入了不检查获取结果的构造器", errors)
    if "DEFINE_GUARD_COND(_name, _checked" not in header:
        fail("实例锁 guard 未通过 checked 构造器保存获取错误", errors)
    if "class_##_name##_release(class_##_name##_t *__guard)" not in header:
        fail("实例锁 guard 缺少显式、可返回错误的释放入口", errors)


def check_call_sites(errors: list[str]) -> None:
    for path in sorted(SRC.glob("*.c")) + sorted(SRC.glob("*.h")):
        source = path.read_text(encoding="utf-8")
        relative = path.relative_to(ROOT)
        if re.search(r"\bguard\s*\(\s*pthread_mutex(?:_[A-Za-z0-9_]+)?\s*\)", source):
            fail(f"{relative} 使用了未检查的 pthread mutex guard", errors)
        discarded_acquire = re.search(
            r"(?m)^\s*(?:\(\s*void\s*\)\s*)?"
            r"(?:pthread_mutex_(?:lock|trylock)|__cleanup_lockdep_mutex_lock)"
            r"\s*\([^;{}]*\)\s*;",
            source,
        )
        if discarded_acquire:
            fail(f"{relative} 丢弃了互斥锁获取结果", errors)

        discarded_unlocks = re.findall(
            r"(?m)^\s*\(\s*void\s*\)\s*"
            r"(?:pthread_mutex_unlock|__cleanup_lockdep_mutex_unlock)\s*\(",
            source,
        )
        if discarded_unlocks and path.name != "logger_cleanup.h":
            fail(f"{relative} 丢弃了互斥锁释放结果", errors)
        if discarded_unlocks and "cleanup 析构只作为早退兜底" not in source:
            fail(f"{relative} 未说明 cleanup 解锁仅是早退兜底", errors)

        acquisitions = Counter(
            re.findall(
                r"\bACQUIRE\s*\(\s*pthread_mutex_[A-Za-z0-9_]*_checked\s*,"
                r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
                source,
            )
        )
        releases = Counter(
            re.findall(
                r"\bRELEASE_ERR\s*\(\s*pthread_mutex_[A-Za-z0-9_]*\s*,"
                r"\s*&\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
                source,
            )
        )
        for guard_name, acquire_count in acquisitions.items():
            release_count = releases[guard_name]
            if release_count < acquire_count:
                fail(
                    f"{relative} 的 {guard_name} 显式释放数少于 checked 获取数"
                    f"（获取 {acquire_count}，释放 {release_count}）",
                    errors,
                )


def check_audit_nested_locks(errors: list[str]) -> None:
    path = SRC / "audit.c"
    lines = path.read_text(encoding="utf-8").splitlines()
    for name in ("operation_lock_nested", "operation_unlock_nested"):
        definition = re.compile(rf"^\s*static\s+int\s+{name}\s*\(")
        call = re.compile(rf"\b{name}\s*\(\s*\)\s*;")
        assignment = re.compile(
            rf"^\s*(?:int\s+)?[A-Za-z_][A-Za-z0-9_]*\s*=\s*"
            rf"{name}\s*\(\s*\)\s*;"
        )
        for number, line in enumerate(lines, start=1):
            if not call.search(line) or definition.search(line):
                continue
            if not assignment.match(line):
                fail(f"audit.c:{number} 未检查 {name}() 的返回值", errors)
    source = "\n".join(lines)
    if re.search(r"\(\s*void\s*\)\s*unlock_scope\s*\(", source):
        fail("audit.c 显式丢弃了 unlock_scope() 的清理结果", errors)


def main() -> int:
    errors: list[str] = []
    check_guard_definitions(errors)
    check_call_sites(errors)
    check_audit_nested_locks(errors)
    if errors:
        for message in errors:
            print(f"错误：{message}", file=sys.stderr)
        return 1
    print("互斥锁错误处理门禁：通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
