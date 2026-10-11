#!/usr/bin/env python3
"""重定位文档门禁的正向与变异检查。"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from check_installed_docs import verify


class InstalledDocsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='docs-check-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.root = self.work / 'relocated prefix' / 'share/doc/prod_c_logger'
        (self.root / 'docs/v2').mkdir(parents=True)
        self.manifest = self.work / 'manifest.txt'
        self.manifest.write_text('README.md\ndocs/v2/README.md\ndocs/v2/SUPPORT_MATRIX.md\n')
        (self.root / 'README.md').write_text('[设计](docs/v2/README.md)\n')
        (self.root / 'docs/v2/README.md').write_text('[支持范围](SUPPORT_MATRIX.md#baseline)\n')
        (self.root / 'docs/v2/SUPPORT_MATRIX.md').write_text('# 基线\n')

    def check(self):
        return verify(self.root, self.manifest)

    def test_relocated_tree_and_transitive_links(self):
        result = self.check()
        self.assertEqual(result['markdown_documents'], 3)
        self.assertEqual(result['relative_file_links'], 2)

    def test_missing_v2_document_fails(self):
        (self.root / 'docs/v2/README.md').unlink()
        with self.assertRaisesRegex(ValueError, '安装文档缺失.*docs/v2/README.md'):
            self.check()

    def test_missing_transitive_link_fails(self):
        (self.root / 'docs/v2/SUPPORT_MATRIX.md').write_text('[缺失目标](../missing.md)\n')
        with self.assertRaisesRegex(ValueError, '相对链接目标缺失或超出文档树：../missing.md'):
            self.check()

    def test_missing_reference_link_fails(self):
        (self.root / 'README.md').write_text('[迁移说明][move]\n\n[move]: missing.md\n')
        with self.assertRaisesRegex(ValueError, '相对链接目标缺失或超出文档树：missing.md'):
            self.check()

    def test_retired_audit_document_is_not_allowed(self):
        (self.root / 'docs/v2/AUDIT_TRANSACTION_SEMANTICS.md').write_text('已退役\n')
        with self.assertRaisesRegex(ValueError, '清单以外.*AUDIT_TRANSACTION_SEMANTICS'):
            self.check()

    def test_link_cannot_escape_to_source(self):
        (self.work / 'source.md').write_text('位于安装树以外的文件\n')
        (self.root / 'README.md').write_text('[源码](../../../source.md)\n')
        with self.assertRaisesRegex(ValueError, '相对链接目标缺失或超出文档树'):
            self.check()

    def test_external_links_fragments_and_code_are_ignored(self):
        (self.root / 'README.md').write_text('''[网站](https://example.com/missing.md)
[本节](#heading)
`[示例](missing.md)`
```md
[示例](missing.md)
```
~~~md
[示例](missing.md)
~~~
''')
        self.assertEqual(self.check()['relative_file_links'], 1)

    def test_percent_encoded_image_and_reference_targets(self):
        (self.root / 'image name.png').write_bytes(b'fixture')
        (self.root / 'README.md').write_text('''![图片](image%20name.png)
[支持范围][matrix]
[matrix]: <docs/v2/SUPPORT_MATRIX.md> "标题"
''')
        self.assertEqual(self.check()['relative_file_links'], 3)

    def test_missing_balanced_parentheses_destination_fails(self):
        (self.root / 'README.md').write_text('[缺失目标](missing(foo).md)\n')
        with self.assertRaisesRegex(ValueError, r'相对链接目标缺失或超出文档树：missing\(foo\).md'):
            self.check()

    def test_existing_nested_and_escaped_parentheses_targets(self):
        (self.root / 'asset(a(b)).png').write_bytes(b'fixture')
        (self.root / 'README.md').write_text(
            '![图片](asset(a(b)).png)\n[图片](asset\\(a\\(b\\)\\).png)\n')
        self.assertEqual(self.check()['relative_file_links'], 3)

    def test_unsafe_or_duplicate_manifest_fails(self):
        for entry in ('../source.md', '/absolute.md', 'README.md'):
            with self.subTest(entry=entry):
                self.manifest.write_text('README.md\n' + entry + '\n')
                with self.assertRaisesRegex(ValueError, '文档路径无效或重复'):
                    self.check()


if __name__ == '__main__':
    unittest.main()
