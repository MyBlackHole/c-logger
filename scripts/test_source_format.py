#!/usr/bin/env python3
"""独立 Git 仓库集成测试；使用与 CI 相同的固定版本格式工具。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check_source_format.py").resolve()
FORMATTER = os.environ.get("CLANG_FORMAT", "clang-format")


class FormatGate(unittest.TestCase):
    def setUp(self):
        output = SCRIPT.parent.parent / "build" / "source-format-tests"
        output.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=output)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-b", "main")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "格式门禁自测")
        self.write(".clang-format", "BasedOnStyle: LLVM\nIndentWidth: 8\nUseTab: Always\n")
        self.write("debt.c", "int   old_debt;\n\n// 未修改分隔\n\nint good;\n")
        self.base = self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root).decode().strip()

    def write(self, path, content):
        (self.root / path).write_text(content)

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "自测提交")
        return self.git("rev-parse", "HEAD")

    def gate(self, expected, base=None, formatter=None, all_sources=False,
             head="HEAD", extra=()):
        outputs = []
        scope = ["--all"] if all_sources else ["--base", base or self.base]
        for language in ("en", "zh_CN"):
            result = subprocess.run([sys.executable, str(SCRIPT), *scope, "--head", head,
                                     "--clang-format", formatter or FORMATTER,
                                     "--lang=" + language, *extra], cwd=self.root,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            outputs.append(result.stdout + result.stderr)
        return outputs[0]

    def clean_debt(self):
        self.write("debt.c", "int good;\n")
        self.commit()

    def fake_formatter(self, response):
        self.write("format tool", "#!/bin/sh\n"
                   "if [ \"$1\" = --version ]; then\n"
                   "  echo 'clang-format version 18.1.8'\n"
                   "elif [ \"$3\" = --dump-config ]; then\n"
                   "  exit 0\n"
                   "else\n" + response + "\nfi\n")
        binary = self.root / "format tool"
        binary.chmod(0o755)
        self.commit()
        return str(binary)

    def test_empty_diff(self):
        self.gate(0)

    def test_documentation_only(self):
        self.write("README.md", "文档\n")
        self.commit()
        self.gate(0)

    def test_added_good(self):
        self.write("new file.c", "int good;\n")
        self.commit()
        self.gate(0)

    def test_added_bad(self):
        self.write("new file.c", "int   bad;\n")
        self.commit()
        self.gate(1)

    def test_modified_good_with_old_debt(self):
        self.write("debt.c", "int   old_debt;\n\n// 未修改分隔\n\nint changed;\n")
        self.commit()
        self.gate(0)

    def test_modified_bad(self):
        self.write("debt.c", "int   old_debt;\n\n// 未修改分隔\n\nint   changed;\n")
        self.commit()
        self.gate(1)

    def test_rename_unchanged_with_spaces(self):
        self.git("mv", "debt.c", "renamed file.c")
        self.commit()
        self.gate(0)

    def test_rename_and_edit(self):
        self.git("mv", "debt.c", "renamed file.c")
        self.write("renamed file.c", "int   old_debt;\n\n// 未修改分隔\n\nint   changed;\n")
        self.commit()
        self.gate(1)

    def test_rename_into_source_scope(self):
        self.write("new.txt", "int   bad;\n")
        self.base = self.commit()
        self.git("mv", "new.txt", "new.c")
        self.commit()
        self.gate(1)

    def test_rename_special_path(self):
        self.git("mv", "debt.c", "quote\" tab\tline\n[1].c")
        self.commit()
        self.gate(0)

    def test_deleted_file(self):
        self.git("rm", "debt.c")
        self.commit()
        self.gate(0)

    def test_deleted_lines(self):
        self.write("debt.c", "int   old_debt;\n\n// 未修改分隔\n\n")
        self.commit()
        self.gate(0)

    def test_unicode_byte_offsets(self):
        self.write("unicode.c", "// 中文\nint   bad;\n")
        self.commit()
        self.gate(1)

    def test_no_final_newline(self):
        self.write("nonl.c", "int   bad;")
        self.commit()
        self.gate(1)

    def test_missing_base_fails(self):
        self.gate(2, base="deadbeef")

    def test_all_zero_base_fails(self):
        self.gate(2, base="0" * 40)

    def test_no_common_ancestor_fails(self):
        self.git("checkout", "--orphan", "unrelated")
        self.git("rm", "-rf", ".")
        self.write(".clang-format", "BasedOnStyle: LLVM\n")
        self.write("unrelated.c", "int unrelated;\n")
        self.commit()
        self.gate(2)

    def test_invalid_config_fails_on_empty_diff(self):
        self.write(".clang-format", "NotAClangFormatOption: true\n")
        self.base = self.commit()
        self.gate(2)

    def test_dirty_tracked_tree_fails(self):
        self.write("debt.c", "int changed;\n")
        self.gate(2)

    def test_symlink_source_fails(self):
        (self.root / "link.c").symlink_to("debt.c")
        self.commit()
        self.gate(2)

    def test_wrong_version_fails(self):
        binary = self.root / "fake-format"
        self.write("fake-format", "#!/bin/sh\necho 'clang-format version 17.0.0'\n")
        binary.chmod(0o755)
        self.commit()
        self.gate(2, formatter=str(binary))

    def test_diverged_base_uses_merge_base(self):
        self.git("checkout", "-qb", "feature")
        self.write("new.c", "int good;\n")
        self.commit()
        self.git("checkout", "-q", "main")
        # 目标分支已整理历史格式；若直接比较其最新提交，会错误地
        # 认为功能分支重新引入历史格式问题，从而产生误报。
        self.write("debt.c", "int old_debt;\n\n// 未修改分隔\n\nint good;\n")
        advanced_base = self.commit()
        self.git("checkout", "-q", "feature")
        self.gate(0, base=advanced_base)

    def test_all_rejects_unchanged_debt(self):
        self.assertIn("debt.c", self.gate(1, all_sources=True))

    def test_all_checks_on_documentation_only_change(self):
        self.write("README.md", "文档\n")
        self.commit()
        self.assertIn("debt.c", self.gate(1, all_sources=True))

    def test_all_checks_after_pure_rename(self):
        self.git("mv", "debt.c", "renamed file.c")
        self.commit()
        self.assertIn("renamed file.c", self.gate(1, all_sources=True))

    def test_all_empty_source_tree(self):
        self.git("rm", "debt.c")
        self.commit()
        self.assertIn("checked 0 source files", self.gate(0, all_sources=True))

    def test_all_ignores_untracked_and_non_source_files(self):
        self.clean_debt()
        self.write("not-source.txt", "int   bad;\n")
        self.commit()
        self.write("untracked.c", "int   bad;\n")
        self.assertIn("checked 1 source files", self.gate(0, all_sources=True))

    def test_all_checks_each_case_insensitive_extension(self):
        self.git("rm", "debt.c")
        for extension in ("c", "h", "cc", "cpp", "cxx", "hh", "hpp", "hxx"):
            for suffix in (extension, extension.upper()):
                self.write("new file." + suffix, "int   bad;\n")
        self.commit()
        self.assertIn("checked 16 source files, 16 failed", self.gate(1, all_sources=True))

    def test_all_legal_c_and_cpp_syntax(self):
        self.clean_debt()
        self.write("atomic.c", "// 中文\n_Atomic(int) value;\n")
        self.write("linkage.cpp", 'extern "C" int value;\n')
        self.commit()
        self.assertIn("checked 3 source files, 0 failed", self.gate(0, all_sources=True))

    def test_untracked_nested_config_cannot_hide_format_debt(self):
        self.clean_debt()
        (self.root / "nested").mkdir()
        self.write("nested/file.c", "int   bad;\n")
        self.commit()
        self.write("nested/.clang-format", "DisableFormat: true\n")
        for all_sources in (False, True):
            with self.subTest(all_sources=all_sources):
                self.assertIn("nested/file.c", self.gate(1, all_sources=all_sources))

    def test_all_special_path(self):
        self.git("mv", "debt.c", "quote\" tab\tline\n[1].C")
        self.commit()
        self.assertIn("need formatting", self.gate(1, all_sources=True))

    def test_all_missing_base_not_required(self):
        self.clean_debt()
        self.gate(0, base="deadbeef", all_sources=True)

    def test_all_and_base_are_mutually_exclusive(self):
        self.assertIn("not allowed", self.gate(2, all_sources=True,
                                              extra=("--base", self.base)))

    def test_missing_config_fails_in_both_modes(self):
        self.git("rm", ".clang-format")
        self.commit()
        for all_sources in (False, True):
            with self.subTest(all_sources=all_sources):
                self.assertIn("tracked regular file", self.gate(2, all_sources=all_sources))

    def test_symlink_config_fails_in_both_modes(self):
        self.git("mv", ".clang-format", "config.yaml")
        (self.root / ".clang-format").symlink_to("config.yaml")
        self.commit()
        for all_sources in (False, True):
            with self.subTest(all_sources=all_sources):
                self.assertIn("tracked regular file", self.gate(2, all_sources=all_sources))

    def test_all_invalid_config_fails(self):
        self.write(".clang-format", "NotAClangFormatOption: true\n")
        self.commit()
        self.assertIn("NotAClangFormatOption", self.gate(2, all_sources=True))

    def test_head_must_be_checked_out_in_both_modes(self):
        self.clean_debt()
        for all_sources in (False, True):
            with self.subTest(all_sources=all_sources):
                self.assertIn("Check out the --head", self.gate(2, head=self.base,
                                                              all_sources=all_sources))

    def test_all_dirty_tracked_tree_fails(self):
        self.write("debt.c", "int changed;\n")
        self.assertIn("Tracked files must be clean", self.gate(2, all_sources=True))

    def test_all_symlink_source_fails(self):
        (self.root / "link.c").symlink_to("debt.c")
        self.commit()
        self.assertIn("Source must be a regular file", self.gate(2, all_sources=True))

    def test_all_missing_tool_fails(self):
        self.assertIn("No such file", self.gate(2, all_sources=True,
                                              formatter=str(self.root / "missing tool")))

    def test_all_wrong_version_fails(self):
        self.write("wrong tool", "#!/bin/sh\necho 'clang-format version 23.0.0'\n")
        binary = self.root / "wrong tool"
        binary.chmod(0o755)
        self.commit()
        self.assertIn("18.1.8 is required", self.gate(2, all_sources=True,
                                                   formatter=str(binary)))

    def test_all_formatter_execution_failure(self):
        binary = self.fake_formatter("exit 7")
        self.assertIn("exit status 7", self.gate(2, all_sources=True, formatter=binary))

    def test_all_invalid_xml_fails(self):
        binary = self.fake_formatter("echo malformed")
        self.assertIn("Source format check failed", self.gate(2, all_sources=True,
                                                            formatter=binary))

    def test_all_incomplete_format_fails(self):
        binary = self.fake_formatter("echo \"<replacements incomplete_format='true'/>\"")
        self.assertIn("Formatting is incomplete", self.gate(2, all_sources=True,
                                                          formatter=binary))


if __name__ == "__main__":
    unittest.main(verbosity=2)
