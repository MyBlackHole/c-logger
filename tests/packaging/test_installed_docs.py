#!/usr/bin/env python3
"""Positive and mutation checks for the relocated documentation gate."""
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
        (self.root / 'README.md').write_text('[design](docs/v2/README.md)\n')
        (self.root / 'docs/v2/README.md').write_text('[support](SUPPORT_MATRIX.md#baseline)\n')
        (self.root / 'docs/v2/SUPPORT_MATRIX.md').write_text('# Baseline\n')

    def check(self):
        return verify(self.root, self.manifest)

    def test_relocated_tree_and_transitive_links(self):
        result = self.check()
        self.assertEqual(result['markdown_documents'], 3)
        self.assertEqual(result['relative_file_links'], 2)

    def test_missing_v2_document_fails(self):
        (self.root / 'docs/v2/README.md').unlink()
        with self.assertRaisesRegex(ValueError, 'missing.*docs/v2/README.md'):
            self.check()

    def test_missing_transitive_link_fails(self):
        (self.root / 'docs/v2/SUPPORT_MATRIX.md').write_text('[missing](../missing.md)\n')
        with self.assertRaisesRegex(ValueError, 'relative link: ../missing.md'):
            self.check()

    def test_missing_reference_link_fails(self):
        (self.root / 'README.md').write_text('[migration][move]\n\n[move]: missing.md\n')
        with self.assertRaisesRegex(ValueError, 'relative link: missing.md'):
            self.check()

    def test_retired_audit_document_is_not_allowed(self):
        (self.root / 'docs/v2/AUDIT_TRANSACTION_SEMANTICS.md').write_text('retired\n')
        with self.assertRaisesRegex(ValueError, 'unexpected.*AUDIT_TRANSACTION_SEMANTICS'):
            self.check()

    def test_link_cannot_escape_to_source(self):
        (self.work / 'source.md').write_text('available outside installation\n')
        (self.root / 'README.md').write_text('[source](../../../source.md)\n')
        with self.assertRaisesRegex(ValueError, 'out-of-tree relative link'):
            self.check()

    def test_external_links_fragments_and_code_are_ignored(self):
        (self.root / 'README.md').write_text('''[site](https://example.com/missing.md)
[here](#heading)
`[example](missing.md)`
```md
[example](missing.md)
```
~~~md
[example](missing.md)
~~~
''')
        self.assertEqual(self.check()['relative_file_links'], 1)

    def test_percent_encoded_image_and_reference_targets(self):
        (self.root / 'image name.png').write_bytes(b'fixture')
        (self.root / 'README.md').write_text('''![image](image%20name.png)
[support][matrix]
[matrix]: <docs/v2/SUPPORT_MATRIX.md> "Title"
''')
        self.assertEqual(self.check()['relative_file_links'], 3)

    def test_unsafe_or_duplicate_manifest_fails(self):
        for entry in ('../source.md', '/absolute.md', 'README.md'):
            with self.subTest(entry=entry):
                self.manifest.write_text('README.md\n' + entry + '\n')
                with self.assertRaisesRegex(ValueError, 'invalid or duplicate'):
                    self.check()


if __name__ == '__main__':
    unittest.main()
