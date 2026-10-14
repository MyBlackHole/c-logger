#!/usr/bin/env python3
"""离线校验 #211 原始证据；可选恢复 ZIP/成员，不执行归档内任何代码。"""
import argparse
import hashlib
import io
import json
import lzma
from pathlib import Path, PurePosixPath
import stat
import tarfile
import zipfile

LIMIT = 2 * 1024 * 1024


def require(condition, code):
    if not condition:
        raise ValueError(code)


def safe_name(name):
    path = PurePosixPath(name)
    require(bool(name) and not path.is_absolute(), "absolute_or_empty_path")
    require(".." not in path.parts and "\\" not in name and ":" not in name,
            "unsafe_path")
    require(path.as_posix() == name and name != ".", "noncanonical_path")
    return path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(root):
    doc = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    require(doc["schema_version"] == 1, "unsupported_schema")
    bundle = doc["bundle"]
    require(bundle["file"] == "artifacts.tar.xz", "unexpected_bundle_name")
    packed = (root / bundle["file"]).read_bytes()
    require(len(packed) == bundle["bytes"] and len(packed) < LIMIT,
            "bundle_size_mismatch")
    require(digest(packed) == bundle["sha256"], "bundle_digest_mismatch")
    # 设置内存和解压大小上限；拒绝拼接压缩流及超大归档。
    decoder = lzma.LZMADecompressor(memlimit=128 * 1024 * 1024)
    raw = decoder.decompress(packed, max_length=LIMIT + 1)
    require(decoder.eof and not decoder.unused_data and len(raw) <= LIMIT,
            "invalid_xz_stream")
    require(len(raw) == bundle["tar_bytes"], "tar_size_mismatch")
    entries = doc["artifacts"]
    require(len(entries) == 5, "artifact_count_mismatch")
    require(all(type(e["artifact_id"]) is int and e["artifact_id"] > 0
                for e in entries), "invalid_artifact_id")
    by_name = {entry["file"]: entry for entry in entries}
    require(len(by_name) == 5, "duplicate_artifact")
    require(len({entry["artifact_id"] for entry in entries}) == 5,
            "duplicate_artifact_id")
    originals, decoded, results = {}, {}, []
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
        members = archive.getmembers()
        require(len(members) == len(by_name), "tar_member_count_mismatch")
        require({m.name for m in members} == set(by_name), "tar_member_set_mismatch")
        for member in members:
            require(member.isfile() and not member.issym() and not member.islnk(),
                    "nonregular_tar_member")
            require(len(safe_name(member.name).parts) == 1, "nested_artifact_path")
            entry = by_name[member.name]
            require(member.size == entry["bytes"] and member.size < LIMIT,
                    "zip_size_mismatch")
            stream = archive.extractfile(member)
            require(stream is not None, "missing_tar_member")
            data = stream.read(LIMIT + 1)
            require(digest(data) == entry["sha256"], "zip_digest_mismatch")
            originals[member.name] = data
            records = {}
            with zipfile.ZipFile(io.BytesIO(data)) as original:
                infos = original.infolist()
                require(len(infos) == entry["member_count"], "zip_member_count_mismatch")
                require(len({i.filename for i in infos}) == len(infos),
                        "duplicate_zip_member")
                require(sum(i.file_size for i in infos) == entry["uncompressed_bytes"],
                        "zip_uncompressed_size_mismatch")
                require(entry["uncompressed_bytes"] <= LIMIT, "oversized_zip")
                for info in infos:
                    safe_name(info.filename)
                    mode = (info.external_attr >> 16) & 0xFFFF
                    require(stat.S_IFMT(mode) in (0, stat.S_IFREG),
                            "nonregular_zip_member")
                    require(not info.is_dir() and not (info.flag_bits & 1),
                            "unsupported_zip_member")
                    require(info.file_size <= LIMIT, "oversized_member")
                    payload = original.read(info)  # 同时校验 ZIP 成员 CRC。
                    require(len(payload) == info.file_size, "member_size_mismatch")
                    records[info.filename] = payload
                require(records["SOURCE_COMMIT"].decode("ascii").strip() ==
                        entry["tested_source_sha"], "source_identity_mismatch")
            decoded[str(entry["artifact_id"])] = records
            results.append({"artifact_id": entry["artifact_id"],
                            "sha256": digest(data), "members": len(records)})
    return originals, decoded, results


def restore(destination, originals, decoded):
    # 校验全部完成后才创建新目录；禁止覆盖既有目标。
    destination.mkdir(parents=True, exist_ok=False)
    for name, data in originals.items():
        with (destination / name).open("xb") as stream:
            stream.write(data)
    for artifact_id, records in decoded.items():
        for name, data in records.items():
            target = destination / artifact_id / safe_name(name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as stream:
                stream.write(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--restore", type=Path)
    args = parser.parse_args()
    try:
        originals, decoded, results = verify(args.root)
        if args.restore:
            restore(args.restore, originals, decoded)
        print(json.dumps({"status": "pass", "artifacts": results,
                          "members": sum(map(len, decoded.values())),
                          "restored": args.restore is not None,
                          "test_replay": "not_run"}, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError,
            zipfile.BadZipFile, lzma.LZMAError) as error:
        # 固定 JSON 键/事件值属于机器输出，不重新解释原始 CI 日志。
        print(json.dumps({"status": "fail", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
