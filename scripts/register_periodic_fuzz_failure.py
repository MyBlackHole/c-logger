#!/usr/bin/env python3
"""将可追溯的周期 fuzz 故障或受控演练登记到 GitHub Issues。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--target", required=True, choices=("public", "format"))
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--source", required=True)
    parser.add_argument("--run-url", required=True)
    parser.add_argument("--drill", action="store_true")
    args = parser.parse_args()
    result = json.loads((args.evidence / "result.json").read_text())
    if result.get("source_commit") != args.source:
        parser.error("source commit does not match result")
    status = result.get("status")
    if args.drill:
        if status != "drill_pass":
            parser.error("drill has not passed")
    elif status not in ("failure_reproduced", "failure_unreproduced",
                        "failure_no_sample"):
        parser.error("result is not a failure")
    if len(args.source) != 40 or any(c not in "0123456789abcdef" for c in args.source):
        parser.error("source must be a full lowercase commit hash")
    if not args.run_url.startswith("https://github.com/"):
        parser.error("run URL must be on github.com")

    sample = result.get("minimized_sha256") or result.get("raw_sha256") or "无；检查原始日志"
    kind = "受控故障演练" if args.drill else "周期 fuzz 失败"
    title = f"{kind}：{args.target}（{status}）"
    body = (f"关联 #212。{kind}，入口 `{args.target}`，状态 `{status}`。\n\n"
            f"- 源码提交：`{args.source}`\n"
            f"- 运行记录：{args.run_url}\n"
            f"- 制品：`periodic-fuzz-{'drill' if args.drill else args.target}`\n"
            f"- 原始输入 SHA-256：`{result.get('raw_sha256', '无')}`\n"
            f"- 最小输入 SHA-256：`{sample}`\n"
            f"- 退出码：运行 `{result.get('run_exit')}`，重放 `{result.get('replay_exit', '未执行')}`\n\n")
    if args.drill:
        body += "这是 UBSan 探针的受控演练，不代表产品缺陷；登记后立即关闭。\n"
    else:
        body += ("请下载制品核对 `result.json` 和原始日志，在相同源码与工具链重放"
                 " `minimized.input`（如存在），用修复 PR 固化最小样本。"
                 "不要仅通过更换 seed 或删除输入关闭本议题。\n")
    body_file = args.evidence / "registration.md"
    body_file.write_text(body)
    created = subprocess.run(["gh", "issue", "create", "--repo", args.repo,
                              "--title", title, "--body-file", str(body_file)],
                             check=True, capture_output=True, text=True)
    url = created.stdout.strip()
    if not url.startswith(f"https://github.com/{args.repo}/issues/"):
        raise ValueError("gh issue create returned an unexpected URL")
    if args.drill:
        subprocess.run(["gh", "issue", "close", url, "--repo", args.repo,
                        "--reason", "completed"], check=True)
    print(url)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"PERIODIC_FUZZ_REGISTRATION_ERROR {error}", file=sys.stderr)
        sys.exit(1)
