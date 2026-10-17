#!/usr/bin/env python3
"""签收一致性门禁：默认拒绝；仅验收标签源码中明确声明的人工决策。

本程序不认证 GitHub 评论作者，也不能替代受保护分支/标签和维护者复核。
它只能防止没有完整签收记录或签收后发生源码漂移的发布自动继续。
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess
import sys

MANIFEST = "validation/release_signoff_v2.json"
ISSUE_MAP = {
    "A_ABI": 100, "B_FORK_HOST": 218, "C_TSAN": 221,
    "D_FUNCTIONAL": 100, "E_ENGINEERING": 210, "F_RELEASE": 100,
    "D1_TYPES": 106, "D2_OWNERSHIP": 106,
    "D3_FAULT_INJECTION": 106, "D4_STYLE": 106,
    "D5_PERFORMANCE": 107, "D6_FUZZ": 212,
}
FIELDS = {"status", "approver", "approved_at", "decision_url",
          "evidence_url", "residual_risk", "tracking_issue"}
ROOT_FIELDS = {"schema", "product", "version", "tag",
               "frozen_base_commit", "decisions"}
SHA = re.compile(r"[0-9a-f]{40}\Z")
TAG = re.compile(r"v2\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
ACTOR = re.compile(r"@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,37})?\Z")
UTC = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\Z")
EVIDENCE = re.compile(
    r"https://github\.com/MyBlackHole/c-logger/"
    r"(?:actions/runs|pull|issues)/[1-9][0-9]*(?:[/?#][^\s]*)?\Z")
BAD_VALUES = ("todo", "tbd", "pending", "placeholder", "待定", "未批准")


class Refused(ValueError):
    pass


def no_duplicate_keys(items):
    result = {}
    for key, value in items:
        if key in result:
            raise Refused("duplicate_json_key:" + key)
        result[key] = value
    return result


def read_manifest(path):
    if path.stat().st_size > 131072:
        raise Refused("manifest_too_large")
    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=no_duplicate_keys)


def git(root, *args):
    result = subprocess.run(["git"] + list(args), cwd=str(root),
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise Refused("git_verification_failed:" + " ".join(args[:2]))
    return result.stdout.strip()


def require_text(text, field, min_length=1):
    if not isinstance(text, str) or len(text.strip()) < min_length:
        raise Refused("missing_or_short:" + field)
    if any(bad in text.lower() for bad in BAD_VALUES):
        raise Refused("placeholder_value:" + field)
    return text.strip()


def validate_decisions(data, version, tag):
    if not isinstance(data, dict) or set(data) != ROOT_FIELDS:
        raise Refused("release_document_shape")
    if data["schema"] != 1 or data["product"] != "c-logger":
        raise Refused("release_document_schema")
    if not TAG.fullmatch(tag) or version != tag[1:]:
        raise Refused("release_tag_version_argument")
    if data["version"] != version or data["tag"] != tag:
        raise Refused("release_manifest_version_tag_mismatch")
    items = data["decisions"]
    if not isinstance(items, dict) or set(items) != set(ISSUE_MAP):
        raise Refused("required_decision_groups_not_exact")
    for group, issue_number in ISSUE_MAP.items():
        record = items[group]
        if not isinstance(record, dict) or set(record) != FIELDS:
            raise Refused("decision_record_shape:" + group)
        if record["status"] not in ("approved", "accepted_with_risk"):
            raise Refused("decision_not_signed:" + group)
        actor = require_text(record["approver"], group + ".approver")
        if not ACTOR.fullmatch(actor) or actor.lower() in (
                "@todo", "@unknown", "@example", "@fixture"):
            raise Refused("invalid_reviewer:" + group)
        stamp = require_text(record["approved_at"], group + ".approved_at")
        if not UTC.fullmatch(stamp):
            raise Refused("non_utc_review_time:" + group)
        try:
            datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError as error:
            raise Refused("invalid_review_time:" + group) from error
        issue = ("https://github.com/MyBlackHole/c-logger/issues/"
                 + str(issue_number))
        if record["tracking_issue"] != issue:
            raise Refused("unexpected_tracking_issue:" + group)
        decision = require_text(record["decision_url"],
                                group + ".decision_url")
        if not re.fullmatch(re.escape(issue) + r"#issuecomment-[1-9][0-9]*",
                            decision):
            raise Refused("decision_not_comment_on_required_issue:" + group)
        evidence = require_text(record["evidence_url"],
                                group + ".evidence_url")
        if not EVIDENCE.fullmatch(evidence):
            raise Refused("evidence_not_linked_to_repository:" + group)
        require_text(record["residual_risk"],
                     group + ".residual_risk", min_length=24)
    return len(items)


def validate_freeze(root, base):
    if not isinstance(base, str) or not SHA.fullmatch(base):
        raise Refused("frozen_base_commit_missing_or_invalid")
    head = git(root, "rev-parse", "--verify", "HEAD")
    if head == base:
        raise Refused("signoff_must_follow_frozen_base")
    git(root, "cat-file", "-e", base + "^{commit}")
    git(root, "merge-base", "--is-ancestor", base, head)
    changed = git(root, "diff", "--no-renames", "--name-only", base, head)
    if changed.splitlines() != [MANIFEST]:
        raise Refused("changes_after_freeze_not_signoff_only")
    git(root, "ls-files", "--error-unmatch", MANIFEST)
    git(root, "diff", "--quiet", "HEAD", "--", MANIFEST)
    git(root, "diff", "--cached", "--quiet", "--", MANIFEST)
    return head


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-tag", required=True)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        root = args.repo_root.resolve(strict=True)
        data = read_manifest(root / MANIFEST)
        count = validate_decisions(data, args.expected_version,
                                   args.expected_tag)
        head = validate_freeze(root, data["frozen_base_commit"])
    except (OSError, ValueError, TypeError) as error:
        print("RELEASE_SIGNOFF_BLOCKED=" + str(error), file=sys.stderr)
        return 1
    print(f"RELEASE_SIGNOFF_VALIDATED tag={args.expected_tag}"
          f" decisions={count} source={head}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
