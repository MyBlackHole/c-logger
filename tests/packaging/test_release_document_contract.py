#!/usr/bin/env python3
"""在发布包基线检查中自动拒绝显式符号数量漂移和过时公开接口状态。"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# 只解析明示当前 ABI 2 符号清单的定点陈述。历史1.x/开发期的数字
# 与“曾有48个”等历史记述不可机械替换。
CURRENT_COUNT = {
    "docs/RELEASE_ENGINEERING.md": (
        r"当前 main 的版本身份为[^\n]*、([0-9]+)符号",
        r"`abi/logger-2\.0\.symbols` 的([0-9]+)符号",
    ),
    "docs/RELEASE_NOTES_2.0.0.md": (
        r"和([0-9]+)个当前公开C符号",
    ),
    "abi/LOGGER_2_0_CONTRACT.md": (
        r"当前生产导出([0-9]+)个",
    ),
}

STALE = {
    "README.md": ("级别设置\n仍待独立迁移", "新 API 迁移验收未完成"),
    "API.md": ("级别设置仍待独立迁移",),
    "abi/LOGGER_2_0_CONTRACT.md": ("级别设置仍单独迁移",),
    "docs/RELEASE_NOTES_2.0.0.md": (
        "级别设置、TLS上下文与脱敏辅助函数的剩余迁移",
        "其余接口迁移与正式发布仍未完成",
    ),
    "TESTING.md": ("本阶段仍不能重建子方运行域",),
}

def current_symbols(root):
    manifest = root / "abi/logger-2.0.symbols"
    symbols = [line.strip() for line in manifest.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.lstrip().startswith("#")]
    if not symbols or len(symbols) != len(set(symbols)):
        raise ValueError("空或重复的公开符号清单")
    return len(symbols)


def doc_errors(docs, count):
    errors = []
    for name, patterns in CURRENT_COUNT.items():
        text = docs[name]
        for pattern in patterns:
            matches = re.findall(pattern, text)
            if len(matches) != 1 or int(matches[0]) != count:
                errors.append((name, pattern, matches))
    for name, markers in STALE.items():
        for marker in markers:
            if marker in docs[name]:
                errors.append((name, "stale", marker))
    return errors


class ReleaseDocumentContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.docs = {name: (ROOT / name).read_text(encoding="utf-8")
                    for name in set(CURRENT_COUNT) | set(STALE)}
        cls.count = current_symbols(ROOT)

    def test_current_abi_2_count_and_status(self):
        self.assertFalse(doc_errors(self.docs, self.count))

    def test_wrong_count_and_stale_contract_are_detected(self):
        modified = dict(self.docs)
        path = "docs/RELEASE_ENGINEERING.md"
        modified[path] = modified[path].replace(
            "、{}符号".format(self.count),
            "、{}符号".format(self.count + 1), 1)
        self.assertTrue(doc_errors(modified, self.count))
        modified = dict(self.docs)
        modified["API.md"] += "\n级别设置仍待独立迁移\n"
        self.assertTrue(doc_errors(modified, self.count))

    def test_installed_preflight_document_is_manifested(self):
        manifest = (ROOT / "docs/INSTALL_MANIFEST.txt").read_text(encoding="utf-8")
        self.assertIn("docs/v2/RELEASE_PREFLIGHT.md\n", manifest)
        self.assertTrue((ROOT / "docs/v2/RELEASE_PREFLIGHT.md").is_file())


if __name__ == "__main__":
    unittest.main()
