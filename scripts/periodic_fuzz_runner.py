#!/usr/bin/env python3
"""运行有预算的 libFuzzer，并保留语料、故障样本和重放证据。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def run(command, log, work, env, wall_seconds, cpu_seconds):
    def set_limits():
        resource.setrlimit(resource.RLIMIT_CPU,
                           (cpu_seconds, cpu_seconds + 1))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    with log.open("wb") as output:
        child = subprocess.Popen(command, cwd=work, env=env, stdout=output,
                                 stderr=subprocess.STDOUT, start_new_session=True,
                                 preexec_fn=set_limits)
        try:
            return child.wait(timeout=wall_seconds)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            output.write(b"\nPERIODIC_FUZZ_WALL_TIMEOUT\n")
            return 124


def copy_seeds(source, destination):
    files = sorted(source.iterdir())
    if not files or any(not path.is_file() or path.is_symlink() for path in files):
        raise ValueError("seed_corpus_requires_regular_files")
    for path in files:
        shutil.copy2(path, destination / path.name)


def deduplicate(source, destination):
    rows = {}
    for path in sorted(source.iterdir()):
        if not path.is_file() or path.is_symlink():
            raise ValueError("corpus_requires_regular_files")
        key = digest(path)
        if key not in rows:
            shutil.copy2(path, destination / key)
            rows[key] = {"sha256": key, "bytes": path.stat().st_size,
                         "sources": []}
        rows[key]["sources"].append(path.name)
    return [rows[key] for key in sorted(rows)]


def git_identity():
    return {"source_commit": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], text=True).strip(),
            "source_tree": subprocess.check_output(
                ["git", "rev-parse", "HEAD^{tree}"], text=True).strip()}


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--seed-corpus", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--seconds", required=True, type=int)
    parser.add_argument("--max-len", required=True, type=int)
    parser.add_argument("--input-timeout", required=True, type=int)
    parser.add_argument("--rss-mb", required=True, type=int)
    parser.add_argument("--drill-input", type=Path)
    parser.add_argument("--expected-diagnostic")
    return parser


def main():
    parser = argument_parser()
    args = parser.parse_args()
    if not 1 <= args.seconds <= 180:
        parser.error("seconds must be in 1..180")
    if not 1 <= args.max_len <= 128:
        parser.error("max-len must be in 1..128")
    if not 1 <= args.input_timeout <= 10:
        parser.error("input-timeout must be in 1..10")
    if not 128 <= args.rss_mb <= 2048:
        parser.error("rss-mb must be in 128..2048")
    if not 1 <= args.seed <= 4294967295:
        parser.error("seed must be in 1..4294967295")
    if bool(args.drill_input) != bool(args.expected_diagnostic):
        parser.error("drill-input and expected-diagnostic must be paired")
    if not args.drill_input and not args.library:
        parser.error("library is required for production fuzz")
    evidence = args.evidence.resolve()
    if evidence == Path("/tmp") or Path("/tmp") in evidence.parents:
        parser.error("evidence must not be under /tmp")
    if evidence.exists():
        parser.error("evidence directory already exists")
    binary = args.binary.resolve(strict=True)
    library = args.library.resolve(strict=True) if args.library else None
    seeds = args.seed_corpus.resolve(strict=True)
    if not seeds.is_dir():
        parser.error("seed-corpus must be a directory")
    drill = args.drill_input.resolve(strict=True) if args.drill_input else None

    evidence.mkdir(parents=True)
    corpus = evidence / "corpus"
    artifacts = evidence / "artifacts"
    unique = evidence / "dedup-corpus"
    work = evidence / "work"
    temp = evidence / "tmp"
    for path in (corpus, artifacts, unique, work, temp):
        path.mkdir()
    copy_seeds(seeds, corpus)
    env = os.environ.copy()
    env["TMPDIR"] = str(temp)
    common = [str(binary), f"-seed={args.seed}",
              f"-max_len={args.max_len}",
              f"-timeout={args.input_timeout}",
              f"-rss_limit_mb={args.rss_mb}",
              f"-artifact_prefix={artifacts}/"]
    if drill:
        command = common + ["-runs=1", str(drill)]
    else:
        command = common + [f"-max_total_time={args.seconds}", str(corpus)]
    run_exit = run(command, evidence / "run.log", work, env,
                   args.seconds + 30, args.seconds + 10)
    run_log = (evidence / "run.log").read_text(errors="replace")
    report = {**git_identity(), "seed": args.seed,
              "budget_seconds": args.seconds,
              "cpu_limit_seconds": args.seconds + 10,
              "max_len": args.max_len,
              "input_timeout": args.input_timeout, "rss_mb": args.rss_mb,
              "binary_sha256": digest(binary),
              "library_sha256": digest(library) if library else None,
              "command": command, "run_exit": run_exit,
              "run_log_sha256": digest(evidence / "run.log"),
              "corpus": deduplicate(corpus, unique)}

    if run_exit == 0:
        report["status"] = "drill_failed" if drill else "pass"
    else:
        samples = sorted(path for path in artifacts.iterdir() if path.is_file()
                         and path.name.startswith(("crash-", "timeout-", "leak-")))
        if samples:
            shutil.copy2(samples[0], evidence / "raw.input")
        elif drill:
            shutil.copy2(drill, evidence / "raw.input")
        if not (evidence / "raw.input").exists():
            report["status"] = "drill_failed" if drill else "failure_no_sample"
        else:
            raw = evidence / "raw.input"
            minimum = evidence / "minimized.input"
            report["raw_sha256"] = digest(raw)
            minimize = common + ["-minimize_crash=1", "-max_total_time=30",
                                 f"-exact_artifact_path={minimum}", str(raw)]
            report["minimize_command"] = minimize
            report["minimize_exit"] = run(minimize, evidence / "minimize.log",
                                          work, env, 45, 35)
            if minimum.is_file():
                report["minimized_sha256"] = digest(minimum)
                replay = common + ["-runs=1", str(minimum)]
                report["replay_command"] = replay
                report["replay_exit"] = run(replay, evidence / "replay.log",
                                            work, env, args.input_timeout + 20,
                                            args.input_timeout + 10)
                replay_log = (evidence / "replay.log").read_text(errors="replace")
                reproduced = report["replay_exit"] not in (0, 124, 137)
            else:
                replay_log = ""
                reproduced = False
            expected = args.expected_diagnostic
            if drill:
                report["status"] = ("drill_pass" if reproduced and
                                    expected in run_log and expected in replay_log
                                    else "drill_failed")
            else:
                report["status"] = ("failure_reproduced" if reproduced else
                                    "failure_unreproduced")
    (evidence / "result.json").write_text(json.dumps(report, ensure_ascii=False,
                                                  indent=2) + "\n")
    print(f"PERIODIC_FUZZ_RESULT status={report['status']} seed={args.seed}"
          f" run_exit={run_exit}")
    return 0 if report["status"] in ("pass", "drill_pass") else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"PERIODIC_FUZZ_ERROR {error}", file=sys.stderr)
        sys.exit(2)
