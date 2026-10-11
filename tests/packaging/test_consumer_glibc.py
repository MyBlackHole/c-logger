#!/usr/bin/env python3
"""Positive and negative final-ELF GLIBC ceiling fixtures (Python 3.8+)."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / 'scripts/check_consumer_glibc.py'
SPEC = importlib.util.spec_from_file_location('consumer_glibc', CHECKER)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)
HEADER = ('  Type: DYN (Shared object file)\n  Class: ELF64\n'
          '  Machine: Advanced Micro Devices X86-64\n')


class ConsumerGlibcTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='logger-consumer-glibc-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.artifact = self.root / 'consumer'
        self.artifact.write_bytes(b'\x7fELF fixture')

    def inspect(self, versions, ceiling='2.31', header=HEADER):
        with patch.object(CHECK, 'run', side_effect=[header, versions]):
            return CHECK.inspect(self.artifact, ceiling)

    def test_equal_and_lower_requirements_pass(self):
        report = self.inspect('Name: GLIBC_2.9\nName: GLIBC_2.31\nName: GLIBC_2.17')
        self.assertTrue(report['passed'])
        self.assertEqual(report['required_glibc_versions'], ['2.9', '2.17', '2.31'])
        self.assertEqual(report['glibc_ceiling'], '2.31')

    def test_newer_and_numeric_order_reject(self):
        for requirement in ('2.34', '2.100', '3.0'):
            with self.subTest(requirement=requirement):
                report = self.inspect('Name: GLIBC_' + requirement)
                self.assertFalse(report['passed'])
                self.assertIn({'glibc_required': requirement, 'ceiling': '2.31'}, report['errors'])

    def test_absent_private_and_non_numeric_requirements_reject(self):
        for versions in ('', 'Name: GLIBC_PRIVATE', 'Name: GLIBC_ABI_DT_RELR',
                         'Name: GLIBC_2.17\nName: GLIBC_ABI_DT_RELR',
                         'Name: GLIBC_2.17\nName: GLIBC_PRIVATE'):
            with self.subTest(versions=versions):
                self.assertFalse(self.inspect(versions)['passed'])

    def test_wrong_architecture_class_and_relocatable_reject(self):
        for header in (HEADER.replace('X86-64', '80386'),
                       HEADER.replace('ELF64', 'ELF32'),
                       HEADER.replace('DYN', 'REL'), ''):
            with self.subTest(header=header):
                self.assertFalse(self.inspect('Name: GLIBC_2.17', header=header)['passed'])
        self.assertTrue(self.inspect('Name: GLIBC_2.17',
                                    header=HEADER.replace('DYN', 'EXEC'))['passed'])

    def test_archive_is_not_a_compatibility_proof(self):
        self.artifact.write_bytes(b'!<arch>\n')
        with self.assertRaisesRegex(ValueError, 'final linked ELF'):
            CHECK.inspect(self.artifact, '2.31')

    def test_module_without_direct_libc_requires_explicit_allowance(self):
        for needed, passed in (('liblogger.so.2', True), ('libc.so.6', False), ('libunrelated.so', False), ('', False)):
            dynamic = '(NEEDED) Shared library: [' + needed + ']' if needed else ''
            with self.subTest(needed=needed), patch.object(
                    CHECK, 'run', side_effect=[HEADER, '', dynamic]):
                report = CHECK.inspect(self.artifact, '2.31', allow_no_glibc_for='liblogger.so.2')
                self.assertEqual(report['passed'], passed)
        with patch.object(CHECK, 'run', side_effect=[HEADER, '',
                '(NEEDED) Shared library: [liblogger.so.2]\n'
                '(NEEDED) Shared library: [libc.so.6]']):
            self.assertFalse(CHECK.inspect(self.artifact, '2.31',
                                          allow_no_glibc_for='liblogger.so.2')['passed'])
        with patch.object(CHECK, 'run', side_effect=[HEADER.replace('DYN', 'EXEC'), '']):
            self.assertFalse(CHECK.inspect(self.artifact, '2.31',
                                          allow_no_glibc_for='liblogger.so.2')['passed'])

    def test_real_linked_consumer_and_cli_fail_closed(self):
        source = self.root / 'consumer.c'
        source.write_text('#include <stdio.h>\nint main(void) { return puts("fixture") < 0; }\n')
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                       [str(source), '-o', str(self.artifact)], check=True,
                       capture_output=True, text=True)
        for ceiling, expected in (('999.0', 0), ('2.0', 1), ('invalid', 2)):
            result = subprocess.run([sys.executable, str(CHECKER), str(self.artifact),
                                     '--max-glibc', ceiling], capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            if expected != 2:
                self.assertEqual(json.loads(result.stdout)['passed'], expected == 0)
        self.artifact.write_text('not an ELF')
        result = subprocess.run([sys.executable, str(CHECKER), str(self.artifact),
                                 '--max-glibc', '2.31'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(json.loads(result.stdout)['passed'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
