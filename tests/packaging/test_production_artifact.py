#!/usr/bin/env python3
"""Regression fixtures for the unconditional production fork-helper exclusion.

Requires a native C compiler, ar, nm, strings and readelf. No fixture is executed.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / 'scripts/check_production_artifact.py'
# Keep explicit expectations here: importing the checker's list would allow a
# mistaken removal in the checker to silently remove its regression coverage.
FORBIDDEN = (
    'fork', 'vfork', 'posix_spawn', 'posix_spawnp', 'logger_fork_reinit',
    'logger_process_thread_count', 'logger_process_arm_clean_fork',
    'logger_process_finish_clean_fork', 'logger_global_stop_for_clean_fork',
    'logger_global_lifetime_broken',
)


class ProductionArtifactForkTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='logger-build-fork-')
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.cc = shlex.split(os.environ.get('CC', 'cc'))

    def artifact(self, source, kind):
        src = self.directory / 'fixture.c'
        src.write_text(source)
        obj = self.directory / 'fixture.o'
        subprocess.run(self.cc + ['-fPIC', '-c', str(src), '-o', str(obj)],
                       check=True, capture_output=True, text=True)
        output = self.directory / ('fixture-fork.a' if kind == 'static' else 'fixture-fork.so')
        if kind == 'static':
            command = ['ar', 'rcs', str(output), str(obj)]
        else:
            command = self.cc + ['-shared', str(obj), '-o', str(output)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        return output

    def inspect(self, artifact, expected):
        result = subprocess.run([sys.executable, str(CHECKER), str(artifact)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['passed'], expected == 0)
        return report

    def test_fork_named_path_and_defensive_atfork_are_allowed(self):
        # Both the parent directory and artifact name include "fork". The
        # actual guard names and pthread_atfork import remain allowed too.
        source = ('extern void pthread_atfork(void);\n'
                  'void logger_process_ensure(void) { pthread_atfork(); }\n'
                  'void logger_process_invalidate_child(void) {}\n'
                  'int logger_process_is_child(void) { return 0; }\n')
        for kind in ('static', 'shared'):
            with self.subTest(kind=kind):
                self.inspect(self.artifact(source, kind), 0)

    def test_each_process_helper_is_rejected(self):
        for symbol in FORBIDDEN:
            for kind in ('static', 'shared'):
                with self.subTest(symbol=symbol, kind=kind):
                    source = f'extern void {symbol}(void);\nvoid probe(void) {{ {symbol}(); }}\n'
                    report = self.inspect(self.artifact(source, kind), 1)
                    self.assertTrue(any(symbol in failure for failure in report['failures']))

    def test_defined_legacy_helper_is_rejected(self):
        for kind in ('static', 'shared'):
            with self.subTest(kind=kind):
                artifact = self.artifact('int logger_fork_reinit(void) { return 0; }\n', kind)
                self.inspect(artifact, 1)

    def test_compiler_clone_of_legacy_helper_is_rejected(self):
        source = ('int clone(void) __asm__("logger_fork_reinit.constprop.0");\n'
                  'int clone(void) { return 0; }\n')
        for kind in ('static', 'shared'):
            with self.subTest(kind=kind):
                self.inspect(self.artifact(source, kind), 1)

    def test_proc_thread_scan_is_rejected(self):
        for kind in ('static', 'shared'):
            with self.subTest(kind=kind):
                artifact = self.artifact('const char path[] = "/proc/self/task";\n', kind)
                report = self.inspect(artifact, 1)
                self.assertIn('/proc/self/task', report['failures'])

    def test_legacy_bypass_option_is_rejected(self):
        artifact = self.artifact('int clean(void) { return 0; }\n', 'static')
        result = subprocess.run([sys.executable, str(CHECKER), str(artifact), '--legacy-fork'],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn('unrecognized arguments: --legacy-fork', result.stderr)


if __name__ == '__main__':
    unittest.main()
