#!/usr/bin/env python3
"""对当前 Logger 生产源码检查少量高风险资源与同步语义。

不是完整 C 解析器，也不替代编译、运行时错误注入或生命周期证明。
扫描所有编译条件分支；只对明确识别的代码模式失败关闭。
"""
from __future__ import annotations

import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

# 保留每个字节的换行位置；注释、字符串和字符常量不参与规则。
NON_CODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)
DISCARDED_WAIT_OR_RWLOCK = re.compile(
    r"(?m)^[ \t]*(?:if\s*\([^;{}]*\)\s*)?"
    r"(?:\(\s*void\s*\)\s*)?"
    r"(?P<name>pthread_rwlock_(?:rdlock|wrlock|tryrdlock|trywrlock|unlock)"
    r"|pthread_cond_(?:wait|timedwait|clockwait)"
    r"|__cleanup_lockdep_rwlock_(?:rdlock|wrlock|trywrlock|unlock))"
    r"\s*\([^;{}]*\)\s*;",
)
DO_BLOCK = re.compile(r"\bdo\s*\{")
WHILE = re.compile(r"\bwhile\s*\(")
CLOSE_CALL = re.compile(r"\bclose\s*\(")
RETRY_ERRNO = re.compile(r"\berrno\b[\s\S]*\bEINTR\b|\bEINTR\b[\s\S]*\berrno\b")


def code_only(text: str) -> str:
    return NON_CODE.sub(
        lambda match: re.sub(r"[^\n]", " ", match.group(0)), text
    )


def matching_end(code: str, opening: int, left: str, right: str) -> int | None:
    depth = 0
    for pos in range(opening, len(code)):
        if code[pos] == left:
            depth += 1
        elif code[pos] == right:
            depth -= 1
            if depth == 0:
                return pos
    return None


def line_number(code: str, position: int) -> int:
    return code.count("\n", 0, position) + 1


def inspect_text(text: str, label: str) -> list[str]:
    code = code_only(text)
    errors: set[str] = set()

    for found in DISCARDED_WAIT_OR_RWLOCK.finditer(code):
        # 真实生产源码允许把赋值表达式换行，例如：
        # int wait_rc =
        #     pthread_cond_wait(...);
        # 仅靠行首匹配会把右值误判成被丢弃的独立语句。
        previous = code[:found.start()].rstrip()
        if previous and (previous[-1] in "=(,+-*/%&|^!?:"
                         or re.search(r"\\breturn$", previous)):
            continue
        errors.add(
            f"{label}:{line_number(code, found.start('name'))}: "
            f"{found.group('name')}() 返回值被丢弃"
        )

    # Linux close(EINTR) 不可重试：描述符可能已经被内核关闭且被别的
    # 线程重新分配。fsync/read/write 的 EINTR 重试属于另一种语义。
    # do {...close(...)} while(errno == EINTR);
    for found in DO_BLOCK.finditer(code):
        brace = code.find("{", found.start(), found.end())
        end = matching_end(code, brace, "{", "}")
        if end is None:
            continue
        suffix = re.match(r"\s*while\s*\(", code[end + 1:])
        if suffix is None:
            continue
        opening = end + 1 + suffix.end() - 1
        closing = matching_end(code, opening, "(", ")")
        if closing is None:
            continue
        if (CLOSE_CALL.search(code[brace + 1:end])
                and RETRY_ERRNO.search(code[opening + 1:closing])):
            errors.add(
                f"{label}:{line_number(code, found.start())}: "
                "禁止 close() 在 EINTR 后循环重试"
            )

    # while(close(fd) < 0 && errno == EINTR) {...}
    # while(errno == EINTR) {close(fd);}
    for found in WHILE.finditer(code):
        opening = code.find("(", found.start(), found.end())
        closing = matching_end(code, opening, "(", ")")
        if closing is None:
            continue
        cond = code[opening + 1:closing]
        if not RETRY_ERRNO.search(cond):
            continue
        if CLOSE_CALL.search(cond):
            errors.add(
                f"{label}:{line_number(code, found.start())}: "
                "禁止 close() 在 EINTR 后循环重试"
            )
            continue
        body_begin = closing + 1
        while body_begin < len(code) and code[body_begin].isspace():
            body_begin += 1
        if body_begin < len(code) and code[body_begin] == "{":
            body_end = matching_end(code, body_begin, "{", "}")
            if body_end is None:
                continue
            body = code[body_begin + 1:body_end]
        else:
            semicolon = code.find(";", body_begin)
            if semicolon < 0:
                continue
            body = code[body_begin:semicolon]
        if CLOSE_CALL.search(body):
            errors.add(
                f"{label}:{line_number(code, found.start())}: "
                "禁止 close() 在 EINTR 后循环重试"
            )
    return sorted(errors)


def check_tree(root: Path) -> list[str]:
    errors: list[str] = []
    for dirname in ("src", "include"):
        directory = root / dirname
        if not directory.is_dir():
            return [f"{dirname}: 生产源码目录不存在"]
        for path in sorted(directory.rglob("*")):
            if path.suffix not in (".c", ".h"):
                continue
            rel = path.relative_to(root).as_posix()
            if path.is_symlink():
                errors.append(f"{rel}: 不允许用符号链接绕过源码语义检查")
                continue
            if path.is_file():
                errors.extend(inspect_text(path.read_text(encoding="utf-8"), rel))
    return errors


def main() -> int:
    errors = check_tree(ROOT)
    for error in errors:
        print(f"错误：{error}", file=sys.stderr)
    if errors:
        return 1
    print("资源与同步语义门禁：通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
