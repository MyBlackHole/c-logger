#!/usr/bin/env python3
"""Check both continuation directions. No timing claims or retries of failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference_build", type=Path)
    parser.add_argument("candidate_build", type=Path)
    args = parser.parse_args()
    def resolve_tool(path):
        path = path.resolve()
        if path.is_file():
            return path
        matches = sorted(
            candidate for candidate in path.rglob("crypto_chain_tool")
            if candidate.is_file()
        )
        if len(matches) != 1:
            parser.error(
                f"expected exactly one crypto_chain_tool under {path}, found {len(matches)}"
            )
        return matches[0]

    tools = [resolve_tool(p) for p in (args.reference_build, args.candidate_build)]
    for algorithm in ("sha256",):
        for first, second in (tools, tools[::-1]):
            with tempfile.TemporaryDirectory(prefix="logger-crypto-cross-") as directory:
                def run(tool, mode):
                    completed = subprocess.run([str(tool), mode, algorithm], cwd=directory,
                                               capture_output=True, text=True, timeout=20, check=True)
                    print(completed.stdout.strip())
                run(first, "append")
                before = (Path(directory) / "app.audit.log").read_bytes()
                run(second, "verify")
                run(second, "append")
                after = (Path(directory) / "app.audit.log").read_bytes()
                if not after.startswith(before) or after.count(b'event="AUDIT_START"') != 2:
                    raise RuntimeError("history changed or lifecycle count invalid")
                run(first, "verify")
                run(second, "verify")
    print("2 SHA-256 cross-version continuation scenarios passed")


if __name__ == "__main__":
    main()
