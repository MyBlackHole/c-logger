#!/usr/bin/env python3
"""用 Clang AST 拒绝 fd 拥有者按值或直接字节复制；不证明跨函数别名/生命周期。"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from tool_i18n import parser_for

ROOT_TYPE = "struct logger_owned_fd"
MEMORY_COPY_ARITY = {
    "memcpy": 3, "memmove": 3,
    "__builtin_memcpy": 3, "__builtin_memmove": 3,
    "__builtin___memcpy_chk": 4, "__builtin___memmove_chk": 4,
}


def walk(node):
    yield node
    for child in node.get("inner", []):
        yield from walk(child)


def value_type(info):
    name = info.get("desugaredQualType", info.get("qualType", ""))
    # 指针/函数指针不拥有所指对象；内嵌数组则随容器一起复制。
    if "*" in name:
        return ""
    name = re.sub(r"\[[^]]*\]", "", name)
    return " ".join(re.sub(r"\b(const|volatile|restrict)\b", "", name).split())


def owning_types(ast):
    nodes = list(walk(ast))
    records = {n["id"]: n for n in nodes
               if n.get("kind") == "RecordDecl" and n.get("completeDefinition")}
    names = {n.get("tagUsed", "struct") + " " + n["name"]: ident
             for ident, n in records.items() if n.get("name")}
    aliases = {}
    for node in nodes:
        if node.get("kind") == "RecordType":
            names.setdefault(value_type(node.get("type", {})),
                             node.get("decl", {}).get("id"))
        elif node.get("kind") == "TypedefDecl":
            aliases[node["name"]] = value_type(node.get("type", {}))

    # Clang不一定为匿名内嵌字段单独输出RecordType；其声明紧邻所属字段。
    # 仅关联拥有定义的同一声明起点，不把指向匿名记录的指针变成拥有者。
    for parent in nodes:
        anonymous = {}
        for child in parent.get("inner", []):
            if child.get("kind") == "RecordDecl" and child.get("completeDefinition") \
                    and not child.get("name"):
                begin = child.get("range", {}).get("begin", {}).get("offset")
                if begin is not None:
                    anonymous[begin] = child["id"]
            if child.get("kind") in ("FieldDecl", "VarDecl"):
                name = value_type(child.get("type", {}))
                begin = child.get("range", {}).get("begin", {}).get("offset")
                if name and begin in anonymous and ("(anonymous" in name or "(unnamed" in name):
                    names.setdefault(name, anonymous[begin])

    def record(info):
        name = value_type(info)
        visited = set()
        while name and name not in names and name in aliases and name not in visited:
            visited.add(name)
            name = aliases[name]
        return names.get(name)

    owned = {names[ROOT_TYPE]} if ROOT_TYPE in names else set()
    while True:
        more = {ident for ident, node in records.items()
                if any(field.get("kind") == "FieldDecl" and
                       record(field.get("type", {})) in owned
                       for field in node.get("inner", []))}
        if more <= owned:
            break
        owned |= more
    return lambda info: record(info) in owned, bool(owned)


def pointee_type(info):
    name = info.get("desugaredQualType", info.get("qualType", ""))
    name = re.sub(r"\b(const|volatile|restrict)\b", "", name).strip()
    # 只解一层指针；指针变量的地址和指针数组仍是借用存储。
    if name.endswith("*"):
        return {"qualType": name[:-1].strip()}
    match = re.fullmatch(r"(.+?)\(\s*\*\s*\)\s*((?:\[[^]]*\]\s*)+)", name)
    if match:
        return {"qualType": match[1] + match[2]}
    return {}


def direct_callee(node):
    while node.get("kind") in ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr",
                               "UnaryOperator"):
        if node["kind"] == "UnaryOperator" and node.get("opcode") not in ("&", "*"):
            return ""
        children = node.get("inner", [])
        if len(children) != 1:
            raise ValueError("invalid_callee_expression")
        node = children[0]
    declaration = node.get("referencedDecl", {}) if node.get("kind") == "DeclRefExpr" else {}
    return declaration.get("name", "") if declaration.get("kind") == "FunctionDecl" else ""


def owned_pointer(node, owns):
    while True:
        if owns(pointee_type(node.get("type", {}))):
            return node
        kind = node.get("kind")
        if kind != "ParenExpr" and not (
                kind in ("ImplicitCastExpr", "CStyleCastExpr") and
                node.get("castKind") in ("NoOp", "BitCast", "LValueToRValue", "ArrayToPointerDecay")):
            return None
        children = node.get("inner", [])
        if len(children) != 1:
            raise ValueError("invalid_pointer_expression")
        # 只回看同一实参的直接指针转换，不追字段基址、取地址对象或变量初值。
        node = children[0]


def inspect_ast(ast):
    if not isinstance(ast, dict) or ast.get("kind") != "TranslationUnitDecl" \
            or not isinstance(ast.get("inner"), list) or not ast["inner"]:
        raise ValueError("invalid_translation_unit")
    owns, seen = owning_types(ast)
    violations = []

    def visit(node, function="", used=True, statement_result=False):
        kind = node.get("kind")
        if kind == "FunctionDecl":
            function = node.get("name", "")
        # sizeof/alignof 不产生值复制；不把未求值操作记作运行时读取。
        if kind == "UnaryExprOrTypeTraitExpr":
            return
        children = node.get("inner", [])
        copy = False
        if kind == "ImplicitCastExpr" and node.get("castKind") == "LValueToRValue" \
                and owns(node.get("type", {})) and used:
            if len(children) != 1:
                raise ValueError("invalid_lvalue_conversion")
            source = children[0]
            while source.get("kind") == "ParenExpr" and len(source.get("inner", [])) == 1:
                source = source["inner"][0]
            # 复合字面量是新对象构造；其成员内的复制仍由递归检查。
            copy = source.get("kind") != "CompoundLiteralExpr"
        if kind == "BinaryOperator" and node.get("opcode") == "=" and used \
                and owns(node.get("type", {})):
            # a = b = move(c) 中内层赋值结果再被复制；没有 LValueToRValue。
            copy = True
        if copy:
            violations.append({"rule": "owned_fd_value_copy", "function": function,
                               "type": node["type"], "range": node.get("range", {})})
        if kind == "CallExpr":
            if not children:
                raise ValueError("invalid_call_expression")
            callee = direct_callee(children[0])
            if callee in MEMORY_COPY_ARITY:
                if len(children) != MEMORY_COPY_ARITY[callee] + 1:
                    raise ValueError("invalid_memory_copy_arguments")
                arguments = []
                for index, argument in enumerate(children[1:3]):
                    pointer = owned_pointer(argument, owns)
                    if pointer is not None:
                        arguments.append({"index": index, "type": pointer["type"],
                                          "range": pointer.get("range", {})})
                # 调用的写入副作用与返回值是否使用无关；不证明长度或重叠安全性。
                if arguments:
                    violations.append({"rule": "owned_fd_memory_copy", "function": function,
                                       "callee": callee, "arguments": arguments,
                                       "range": node.get("range", {})})
        if kind == "GenericSelectionExpr":
            # 控制表达式及未选分支不求值；只检查被选关联。
            children = [c for c in children if c.get("selected")]
        for index, child in enumerate(children):
            child_used = True
            if kind == "CompoundStmt":
                child_used = statement_result and index == len(children) - 1
            elif kind == "BinaryOperator" and node.get("opcode") in ("=", ","):
                child_used = index == 1 and (used or node["opcode"] == "=")
            elif kind in ("ParenExpr", "GenericSelectionExpr"):
                child_used = used
            elif kind == "CStyleCastExpr" and node.get("castKind") == "ToVoid":
                child_used = False
            elif kind in ("IfStmt", "WhileStmt"):
                child_used = index == 0
            elif kind == "DoStmt":
                child_used = index != 0
            elif kind == "ForStmt":
                child_used = index == 2
            elif kind in ("LabelStmt", "CaseStmt", "DefaultStmt"):
                child_used = index != len(children) - 1
            visit(child, function, child_used, kind == "StmtExpr" and used)

    visit(ast)
    return violations, seen


def analyze(compiler, flags, source, cwd, timeout=30):
    command = [compiler] + flags + ["-Xclang", "-ast-dump=json", "-fsyntax-only", source]
    result = subprocess.run(command, cwd=cwd, capture_output=True,
                            timeout=timeout, env=dict(os.environ, LC_ALL="C"))
    # 编译器失败不是“成功发现非法复制”；任何诊断亦不得静默吞掉。
    if result.returncode or result.stderr:
        raise ValueError("clang_diagnostic:" + result.stderr.decode("utf-8", "replace")[:4000])
    ast = json.loads(result.stdout)
    violations, seen = inspect_ast(ast)
    return {"source": source, "command": command,
            "source_sha256": hashlib.sha256((Path(cwd) / source).read_bytes()).hexdigest(),
            "ast_sha256": hashlib.sha256(result.stdout).hexdigest(),
            "owner_type_seen": seen, "violations": violations}


def run_manifest(manifest):
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 \
            or not isinstance(manifest.get("units"), list) \
            or not manifest["units"]:
        raise ValueError("invalid_manifest")
    cwd = str(Path(manifest["directory"]).resolve(strict=True))
    results, keys = [], set()
    for unit in manifest["units"]:
        key = (unit["target"], unit["source"])
        flags = unit["flags"]
        if key in keys or not isinstance(flags, list) or not all(isinstance(f, str) for f in flags):
            raise ValueError("invalid_or_duplicate_unit")
        keys.add(key)
        source = Path(unit["source"])
        if source.suffix != ".c" or not (Path(cwd) / source).is_file():
            raise ValueError("missing_c_source")
        version = subprocess.run([unit["compiler"], "--version"], cwd=cwd,
                                 capture_output=True, text=True, timeout=10, check=True)
        if not re.search(r"clang version \d+", version.stdout):
            raise ValueError("clang_required")
        result = analyze(unit["compiler"], flags, unit["source"], cwd)
        result.update(target=unit["target"], compiler_version=version.stdout.splitlines()[0])
        results.append(result)
    if not any(r["owner_type_seen"] for r in results):
        raise ValueError("owner_type_not_observed")
    return {"schema": 1, "passed": not any(r["violations"] for r in results),
            "units_checked": len(results), "results": results}


def main():
    parser, _ = parser_for("Check typed file-descriptor ownership copies with Clang AST.")
    parser.add_argument("--manifest", required=True)
    args = parser.parse_args()
    try:
        report = run_manifest(json.loads(Path(args.manifest).read_text(encoding="utf-8")))
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RecursionError,
            subprocess.SubprocessError) as error:
        print(json.dumps({"schema": 1, "passed": False, "error": str(error)}, ensure_ascii=False))
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
