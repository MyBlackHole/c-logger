#!/usr/bin/env python3
"""Check the real Xmake VERSION guard in isolated local source copies."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument('--xmake', default='xmake')
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
XMAKE = shutil.which(ARGS.xmake)
if XMAKE is None:
    PARSER.error('Xmake executable not found: ' + ARGS.xmake)


class VersionIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='logger-version-identity-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.source = Path(cls.temp.name) / 'source'
        shutil.copytree(ROOT, cls.source,
                        ignore=shutil.ignore_patterns('.git', '.xmake', '__pycache__',
                                                     'build', 'build-*'))
        cls.version = (ROOT / 'VERSION').read_text().strip()

    def setUp(self):
        (self.source / 'VERSION').write_text(self.version + '\n')
        self.build = Path(self.temp.name) / self.id().rsplit('.', 1)[-1]

    def configure(self, exported, source_version=None):
        if source_version is not None:
            (self.source / 'VERSION').write_text(source_version)
        env = dict(os.environ)
        env.pop('LOGGER_PROJECT_VERSION', None)
        env.pop('XMAKE_PROJECT_DIR', None)
        if exported is not None:
            env['LOGGER_PROJECT_VERSION'] = exported
        return subprocess.run([XMAKE, 'f', '-c', '-m', 'release', '-o', str(self.build),
                               '--build_shared=n', '--build_tests=n',
                               '--build_private_tests=n', '--build_regression_tests=n'],
                              cwd=self.source, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=20)

    def assert_rejected(self, result, diagnostic):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(diagnostic, result.stdout)
        self.assertFalse(list(self.build.rglob('LoggerConfig.cmake')))

    def test_missing_export_is_rejected(self):
        self.assert_rejected(self.configure(None), 'export LOGGER_PROJECT_VERSION')

    def test_invalid_export_is_rejected(self):
        self.assert_rejected(self.configure('invalid'), 'export LOGGER_PROJECT_VERSION')

    def test_stale_v1_export_is_rejected(self):
        self.assert_rejected(self.configure('1.0.0'),
                             'LOGGER_PROJECT_VERSION must exactly match VERSION')

    def test_other_numeric_export_is_rejected(self):
        major, minor, patch = map(int, self.version.split('.'))
        other_version = '{}.{}.{}'.format(major, minor, patch + 1)
        self.assert_rejected(self.configure(other_version),
                             'LOGGER_PROJECT_VERSION must exactly match VERSION')

    def test_invalid_source_version_is_rejected(self):
        self.assert_rejected(self.configure(self.version, '2.0.0-dev\n'),
                             'VERSION must contain MAJOR.MINOR.PATCH')

    def test_matching_export_is_accepted(self):
        result = self.configure(self.version)
        self.assertEqual(result.returncode, 0, result.stdout)
        configs = list(self.build.rglob('LoggerConfig.cmake'))
        self.assertEqual(len(configs), 1)
        self.assertIn('set(Logger_VERSION "' + self.version + '")', configs[0].read_text())

    def test_source_whitespace_is_trimmed(self):
        result = self.configure(self.version, ' \n' + self.version + '\n\t')
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + UNITTEST_ARGS)
