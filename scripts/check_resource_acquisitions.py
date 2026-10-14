#!/usr/bin/env python3
"""#106：审核生产源码的资源获取调用及其所有权/失败验证清单。

这是审查清单的词法门禁，不是 C 语义分析器；不证明测试实际命中该行。
新增/删除/更改获取点必须同步审查所有者、错误回滚和测试证据。
"""
from __future__ import annotations

from collections import Counter
import argparse
import csv
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path("validation/resource_acquisitions.tsv")
FIELDS = ("file", "api", "arguments", "count", "owner",
          "failure_test", "coverage", "rationale")

# 明确枚举取得所有权或建立必须销毁的同步对象的入口。
# 这不是所有系统调用的清单：write、fsync、flock、close 另有独立合同。
ACQUISITIONS = (
    "malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign",
    "strdup", "strndup", "open", "openat", "creat", "fopen", "fdopen",
    "opendir", "fdopendir", "socket", "socketpair", "accept", "accept4",
    "pipe", "pipe2", "dup", "dup2", "dup3", "mmap", "mremap",
    "eventfd", "epoll_create", "epoll_create1", "timerfd_create",
    "inotify_init", "inotify_init1", "memfd_create", "mkstemp", "mkostemp",
    "pthread_create", "pthread_mutex_init", "pthread_cond_init",
    "pthread_rwlock_init", "pthread_spin_init", "pthread_key_create",
    "pthread_attr_init", "sem_init", "getaddrinfo",
)
CALL = re.compile(r"\b(" + "|".join(ACQUISITIONS) + r")\s*\(")
NON_CODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def hide_noncode(source: str) -> str:
    # 保持每个字符的偏移量和换行位置，供精确识别与错误定位。
    return NON_CODE.sub(
        lambda match: re.sub(r"[^\n]", " ", match.group()), source
    )


def arguments(source: str) -> str:
    # 参数规范化时保留字符串常量：openat(fd, ".") != openat(fd, "x")。
    # 注释不影响参数身份；仅移除字符串字面量以外的空白。
    source = COMMENT.sub(" ", source)
    result = []
    pos = 0
    for match in LITERAL.finditer(source):
        result.append(re.sub(r"\s+", "", source[pos:match.start()]))
        result.append(match.group())
        pos = match.end()
    result.append(re.sub(r"\s+", "", source[pos:]))
    return "".join(result)


def closing_paren(code: str, opening: int) -> int | None:
    depth = 0
    for position in range(opening, len(code)):
        char = code[position]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return position
    return None


def scan(root: Path) -> tuple[Counter, list[str]]:
    observed: Counter = Counter()
    errors: list[str] = []
    for directory in ("src", "include"):
        base = root / directory
        if not base.is_dir():
            errors.append(f"生产源码目录缺失：{directory}")
            continue
        for path in sorted(base.rglob("*")):
            if path.is_symlink() and (path.suffix in (".c", ".h") or
                                      path.name.endswith(".h.in")):
                errors.append(f"{path.relative_to(root)}: 不允许符号链接")
                continue
            if not path.is_file() or not (path.suffix in (".c", ".h") or
                                           path.name.endswith(".h.in")):
                continue
            relative = path.relative_to(root).as_posix()
            original = path.read_text(encoding="utf-8")
            code = hide_noncode(original)
            for match in CALL.finditer(code):
                opening = code.find("(", match.start(), match.end())
                closing = closing_paren(code, opening)
                line = code.count("\n", 0, match.start()) + 1
                if closing is None:
                    errors.append(f"{relative}:{line}: 未闭合的资源获取调用")
                    continue
                key = (
                    relative, match.group(1),
                    arguments(original[opening + 1:closing]),
                )
                observed[key] += 1
    return observed, errors


def inventory(root: Path) -> Counter:
    manifest = root / MANIFEST
    expected: Counter = Counter()
    with manifest.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if tuple(reader.fieldnames or ()) != FIELDS:
            raise ValueError("资源获取清单的表头无效")
        for line, item in enumerate(reader, 2):
            if set(item) != set(FIELDS) or not all(
                value and value.strip() for value in item.values()
            ):
                raise ValueError(f"资源获取清单第 {line} 行有缺失字段")
            key = tuple(item[name] for name in FIELDS[:3])
            if key in expected:
                raise ValueError(f"资源获取清单第 {line} 行存在重复调用")
            if item["api"] not in ACQUISITIONS:
                raise ValueError(f"资源获取清单第 {line} 行包含未知获取 API")
            try:
                count = int(item["count"])
            except ValueError as error:
                raise ValueError(f"资源获取清单第 {line} 行计数非法") from error
            if count < 1 or str(count) != item["count"]:
                raise ValueError(f"资源获取清单第 {line} 行计数非法")
            if item["coverage"] not in ("direct", "adjacent"):
                raise ValueError(f"资源获取清单第 {line} 行验证等级非法")
            if len(item["owner"].strip()) < 4 or len(item["rationale"].strip()) < 8:
                raise ValueError(f"资源获取清单第 {line} 行缺少可审查理由")
            evidence = Path(item["failure_test"])
            if (evidence.is_absolute() or ".." in evidence.parts or
                not evidence.parts or evidence.parts[0] != "tests" or
                evidence.suffix not in (".c", ".py") or
                not (root / evidence).is_file() or
                (root / evidence).is_symlink()):
                raise ValueError(
                    f"资源获取清单第 {line} 行测试证据不是仓库内已有的测试文件"
                )
            expected[key] = count
    if not expected:
        raise ValueError("资源获取清单为空")
    return expected


def check(root: Path) -> tuple[int, list[str]]:
    observed, errors = scan(root)
    expected = inventory(root)
    for key, count in sorted((observed - expected).items()):
        errors.append(
            f"新增或变更且未评审的资源获取（{count}）：{' | '.join(key)}"
        )
    for key, count in sorted((expected - observed).items()):
        errors.append(
            f"原有资源获取已缺失或签名变更（{count}）：{' | '.join(key)}"
        )
    return sum(observed.values()), errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        count, errors = check(args.root)
    except (OSError, ValueError, csv.Error) as error:
        print(f"资源获取门禁：{error}", file=sys.stderr)
        return 1
    if errors:
        for message in errors:
            print(message, file=sys.stderr)
        return 1
    print(f"资源获取门禁：{count} 处获取调用均有审查身份与测试证据")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
