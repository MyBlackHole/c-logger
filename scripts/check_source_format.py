#!/usr/bin/env python3
"""检查已提交 Git 快照中的完整 C 系列源码或修改行，不写回文件。"""
import bisect
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

from tool_i18n import parser_for

VERSION = "18.1.8"
EXTENSIONS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"}


def run(*args, data=None):
    return subprocess.run(args, input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout


def git(*args):
    return run("git", *args)


def commit(ref):
    # 先解析为提交 SHA，避免把传入引用解释成 Git 选项。
    # 历史缺失或浅克隆时必须失败，不能静默退回 HEAD。
    return git("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").decode().strip()


def changes(base, head):
    fields = git("diff", "--name-status", "-z", "--find-renames", base, head, "--").split(b"\0")
    index = 0
    while index < len(fields) - 1:
        status = fields[index].decode("ascii")
        old = new = os.fsdecode(fields[index + 1])
        index += 2
        if status.startswith(("R", "C")):
            new = os.fsdecode(fields[index])
            index += 1
        if status != "D" and Path(new).suffix.lower() in EXTENSIONS:
            yield status, old, new


def sources(head):
    for path in git("ls-tree", "-r", "--name-only", "-z", head).split(b"\0"):
        path = os.fsdecode(path)
        if Path(path).suffix.lower() in EXTENSIONS:
            yield None, None, path


def file_mode(head, path):
    return git("--literal-pathspecs", "ls-tree", head, "--", path).split(b" ", 1)[0]


def changed_lines(old, new):
    result = set()
    for tag, _, _, start, end in difflib.SequenceMatcher(
            None, old.splitlines(keepends=True), new.splitlines(keepends=True),
            autojunk=False).get_opcodes():
        if tag in ("insert", "replace"):
            result.update(range(start + 1, end + 1))
    return result


def affected_lines(data, offset, length):
    # Clang 偏移量以字节计，不是 Unicode 字符索引。
    # 跨行边界的空白也属于后一行的缩进。
    starts = [0] + [m.end() for m in re.finditer(b"\n", data)]
    first = bisect.bisect_right(starts, offset)
    last = bisect.bisect_right(starts, offset + length)
    return range(first, last + 1)


def check(base_ref, head_ref, formatter, _=lambda message: message):
    version = run(formatter, "--version").decode().strip()
    if not re.search(r"\bversion " + re.escape(VERSION) + r"(?:\s|$)", version):
        raise ValueError(_("clang-format {required} is required; found {actual}").format(required=VERSION, actual=version))
    head = commit(head_ref)
    base = commit(base_ref) if base_ref is not None else None
    if commit("HEAD") != head:
        raise ValueError(_("Check out the --head commit first to use its .clang-format"))
    if git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError(_("Tracked files must be clean; commit changes before checking"))
    if base is not None:
        base = git("merge-base", base, head).decode().strip()
    # 两种模式都固定使用当前提交的根配置，避免未跟踪的子目录配置
    # 覆盖项目规则；配置缺失时不能由 fallback-style=none 静默跳过。
    if file_mode(head, ".clang-format") not in (b"100644", b"100755"):
        raise ValueError(_("Project .clang-format must be a tracked regular file"))
    style = "--style=file:" + str(Path(".clang-format").absolute())
    run(formatter, style, "--fallback-style=none", "--dump-config")
    failures = 0
    checked = 0
    entries = sources(head) if base is None else changes(base, head)
    for status, old_path, path in entries:
        if file_mode(head, path) not in (b"100644", b"100755"):
            raise ValueError(_("Source must be a regular file: {path!r}").format(path=path))
        new = git("show", f"{head}:{path}")
        if base is not None:
            new_source = (status == "A" or status.startswith("C")
                          or Path(old_path).suffix.lower() not in EXTENSIONS)
            old = b"" if new_source else git("show", f"{base}:{old_path}")
            lines = changed_lines(old, new)
            if not lines:
                continue
        checked += 1
        xml = ET.fromstring(run(formatter, style, "--fallback-style=none",
                                "--assume-filename=" + str(Path(path).absolute()),
                                "--output-replacements-xml", data=new))
        if xml.attrib.get("incomplete_format") == "true":
            raise ValueError(_("Formatting is incomplete: {path!r}").format(path=path))
        bad = set()
        for replacement in xml.findall("replacement"):
            affected = affected_lines(new, int(replacement.attrib["offset"]),
                                      int(replacement.attrib["length"]))
            bad.update(affected if base is None else lines.intersection(affected))
        if bad:
            failures += 1
            message = (_("{path!r}: lines need formatting: {lines}") if base is None else
                       _("{path!r}: changed lines need formatting: {lines}"))
            print(message.format(path=path, lines=','.join(map(str, sorted(bad)))))
    if base is None:
        print(_("clang-format {version}: checked {checked} source files, {failures} failed; commit {head}").format(version=VERSION, checked=checked, failures=failures, head=head))
    else:
        print(_("clang-format {version}: checked {checked} changed source files, {failures} failed; merge base {base}").format(version=VERSION, checked=checked, failures=failures, base=base))
    return bool(failures)


def main():
    parser, _ = parser_for("Check C-family formatting in committed Git snapshots without modifying files.")
    scope = parser.add_mutually_exclusive_group(required=True)
    scope.add_argument("--all", action="store_true", help=_("Check every tracked C-family source file in --head"))
    scope.add_argument("--base", help=_("PR target branch SHA/ref; check changed lines from the merge base"))
    parser.add_argument("--head", default="HEAD", help=_("Commit to check (default: HEAD)"))
    parser.add_argument("--clang-format", default="clang-format", help=_("Path to the clang-format executable"))
    args = parser.parse_args()
    try:
        os.chdir(git("rev-parse", "--show-toplevel").decode().strip())
        return int(check(args.base, args.head, args.clang_format, _))
    except (ValueError, OSError, subprocess.CalledProcessError, ET.ParseError) as error:
        print(_("Source format check failed: {error}").format(error=error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr.decode(errors="replace"), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
