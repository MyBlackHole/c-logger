#!/usr/bin/env python3
"""核对原始#106/#107矩阵与当前主线增量的状态边界。"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MATRIX = ROOT / "validation" / "acceptance-210"


def audit(readme, issue106, issue107, delta, symbols):
    errors = []
    for title, content in (("README", readme), ("ISSUE_106", issue106),
                           ("ISSUE_107", issue107), ("CURRENT_DELTA", delta)):
        if not content.startswith("# "):
            errors.append(title + ": missing heading")
    for item in range(1, 7):
        token = "D" + str(item)
        if not re.search(r"^\\| " + token + r"\\s", delta, re.MULTILINE):
            errors.append("missing decision " + token)
    rows106 = re.findall(r"^\| (?:T[1-7]|S[1-8]|C(?:10|[1-9])|F(?:1[0-2]|[1-9])|A[1-7]) \|", issue106, re.MULTILINE)
    rows107 = re.findall(r"^\| (?:W[1-9]|U[1-5]|Z[1-4]|F[1-8]|P[1-6]|C[1-7]|B[1-9]|A[1-9]) \|", issue107, re.MULTILINE)
    if len(rows106) != 44 or len(rows107) != 57:
        errors.append("original row mapping changed: {}/{}".format(len(rows106), len(rows107)))
    symbols = [line.strip() for line in symbols.splitlines()
               if line.strip() and not line.lstrip().startswith("#")]
    if not symbols or len(symbols) != len(set(symbols)):
        errors.append("invalid current symbol list")
    if "当前确认：{} 导出".format(len(symbols)) not in issue107:
        errors.append("U5 current symbols differ from manifest")
    if "D1–D6" not in delta or "尚未批准" not in delta:
        errors.append("unapproved scope disclaimer is missing")
    if "31项TSan" not in delta or "921实际通过" not in delta:
        errors.append("historical detector skip boundary has drifted")
    return errors


class AcceptanceMatrixContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.documents = [(MATRIX / name).read_text(encoding="utf-8")
                         for name in ("README.md", "ISSUE_106.md",
                                      "ISSUE_107.md", "CURRENT_DELTA.md")]
        cls.manifest = (ROOT / "abi/logger-2.0.symbols").read_text(encoding="utf-8")

    def test_current_matrix_and_source_identity(self):
        self.assertEqual([], audit(*self.documents, self.manifest))

    def test_missing_decision_is_rejected(self):
        changed = list(self.documents)
        changed[3] = changed[3].replace("| D4 风格语义 |", "| unknown 风格语义 |", 1)
        self.assertTrue(audit(*changed, self.manifest))

    def test_stale_abi_and_detector_summary_are_rejected(self):
        changed = list(self.documents)
        changed[2] = changed[2].replace("当前确认：43 导出",
                                        "当前确认：47 导出", 1)
        self.assertTrue(audit(*changed, self.manifest))
        changed = list(self.documents)
        changed[3] = changed[3].replace("921实际通过", "952实际通过", 1)
        self.assertTrue(audit(*changed, self.manifest))


if __name__ == "__main__":
    unittest.main()
