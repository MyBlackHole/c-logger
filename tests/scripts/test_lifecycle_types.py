#!/usr/bin/env python3
"""用 Xmake 当前 C 编译器核对新接口正例与已移除接口误用。"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    prefix = '#include "logger.h"\n#include "console.h"\n'
    cases = {
        "create-signature": (
            'void f(void) { logger_t *l = logger_create(NULL); (void)l; }',
            'void f(void) { logger_t *l = NULL; (void)logger_create(NULL, &l); }',
            "logger_create"),
        "destroy-owner": (
            'void f(logger_t *l) { (void)logger_destroy(l); }',
            'void f(logger_t *l) { (void)logger_destroy(&l); }',
            "logger_destroy"),
        "removed-status": (
            'void f(logger_t *l) { (void)logger_destroy_status(l); }',
            'void f(logger_t *l) { (void)logger_destroy(&l); }',
            "logger_destroy_status"),
    }
    for symbol, call in (("logger_init", "logger_init(NULL)"),
                         ("logger_shutdown", "logger_shutdown()"),
                         ("logger_shutdown_status", "logger_shutdown_status()")):
        cases["removed-" + symbol] = (
            "void f(void) { (void)" + call + "; }",
            "void f(void) { (void)logger_default_set(NULL, NULL); }", symbol)
    for symbol, call in (("logger_flush_status", "logger_flush_status()"),
                         ("logger_flush_instance", "logger_flush_instance(NULL)"),
                         ("logger_flush_instance_status", "logger_flush_instance_status(NULL)"),
                         ("logger_reopen_instance", "logger_reopen_instance(NULL)")):
        cases["removed-" + symbol] = (
            "void f(void) { (void)" + call + "; }",
            "void f(void) { (void)logger_flush(NULL); }", symbol)
    for symbol in ("logger_flush", "logger_reopen"):
        cases["explicit-" + symbol] = (
            "void f(void) { (void)" + symbol + "(); }",
            "void f(void) { (void)" + symbol + "(NULL); }", symbol)
        cases["status-" + symbol] = (
            "void (*f)(logger_t *) = " + symbol + ";",
            "int (*f)(logger_t *) = " + symbol + ";", symbol)
    for symbol, kind in (("console_init", "const console_config_t *"),
                         ("console_set_verbosity", "console_verbosity_t"),
                         ("console_set_color", "console_color_mode_t")):
        cases["status-" + symbol] = (
            "void (*f)(" + kind + ") = " + symbol + ";",
            "int (*f)(" + kind + ") = " + symbol + ";", symbol)
    for symbol in ("console_stdout_is_tty", "console_stderr_is_tty"):
        cases["output-" + symbol] = (
            "void f(void) { (void)" + symbol + "(); }",
            "void f(void) { int tty; (void)" + symbol + "(&tty); }", symbol)
        cases["type-" + symbol] = (
            "int (*f)(void) = " + symbol + ";",
            "int (*f)(int *) = " + symbol + ";", symbol)
    env = dict(os.environ, LC_ALL="C")
    with tempfile.TemporaryDirectory(prefix="logger-api-types-") as folder:
        path = Path(folder) / "contract.c"
        for name, (bad, good, symbol) in cases.items():
            command = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Wpedantic",
                       "-Werror", "-fsyntax-only", "-I" + str(root / "include"),
                       str(path)]
            # 先构建只修正类型的正例，拒绝把工具/依赖缺失算成负例成功。
            path.write_text(prefix + good + "\n")
            positive = subprocess.run(command, env=env, capture_output=True, text=True)
            if positive.returncode:
                print(positive.stdout + positive.stderr)
                return 1
            path.write_text(prefix + bad + "\n")
            negative = subprocess.run(command, env=env, capture_output=True, text=True)
            if negative.returncode == 0 or symbol not in negative.stderr:
                print(negative.stdout + negative.stderr)
                return 1
            print("LIFECYCLE_TYPE_PASS=" + name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
