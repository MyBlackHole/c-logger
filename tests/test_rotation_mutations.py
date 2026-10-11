#!/usr/bin/env python3
"""Exercise the unchanged rotation test against replacement implementations."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
args = parser.parse_args()
binary = args.binary.resolve(strict=True)
with tempfile.TemporaryDirectory(prefix='logger-rotation-mutations-') as temporary:
    cwd = Path(temporary)
    for stale in (False, True):
        if stale:
            (cwd / 'rotation.fakeZ.log').write_text('stale archive\n')
            (cwd / 'rotation.log').write_text('stale active\n')
        for mode, expected in (('valid', 0), ('no-output', 3), ('active-only', 3),
                               ('empty-archive', 3), ('missing', 3), ('duplicate', 3), ('corrupt', 3),
                               ('reordered', 3), ('reversed-files', 3), ('shutdown-error', 2)):
            result = subprocess.run([str(binary)], cwd=cwd,
                                    env=dict(os.environ, LOGGER_ROTATION_MUTATION=mode),
                                    capture_output=True, text=True, timeout=10)
            if result.returncode != expected:
                raise RuntimeError((stale, mode, result.returncode, result.stdout, result.stderr))
            if list(cwd.glob('rotation-test.*')):
                raise RuntimeError('private directory leaked')
            if stale:
                if ((cwd / 'rotation.fakeZ.log').read_text() != 'stale archive\n' or
                        (cwd / 'rotation.log').read_text() != 'stale active\n'):
                    raise RuntimeError('stale parent files changed')
            print(f'stale={stale} mutation={mode}: exit={result.returncode} expected={expected}')
