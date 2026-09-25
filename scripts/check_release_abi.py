#!/usr/bin/env python3
"""Inspect a Linux ELF release without executing it. Requires readelf and nm.
This proves exported-name/version/ELF metadata properties, NOT behavioral ABI
compatibility, minimum-kernel support, durable storage, or platform certification.
"""
import argparse
import json
import os
import re
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          env=dict(os.environ, LC_ALL='C')).stdout


def inspect(artifact, manifest, legacy=False, max_glibc=None, machine=None, elf_class=None):
    names = {s.strip() for s in manifest.read_text().splitlines()
             if s.strip() and not s.lstrip().startswith('#')}
    if legacy:
        names.add('logger_fork_reinit')
    errors = []
    if artifact.read_bytes()[:8] == b'!<arch>\n':
        raise ValueError('Check the final shared ELF/consumer for GLIBC requirements; archives have no final dynamic ABI')
    dynamic = run('readelf', '-dW', str(artifact))
    symbols = run('nm', '-D', '--defined-only', str(artifact))
    version_info = run('readelf', '--version-info', '-W', str(artifact))
    header = run('readelf', '-hW', str(artifact))
    exported = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        full = parts[-1]
        if full == 'LOGGER_0.9' and parts[-2] == 'A':
            continue
        name = full.split('@', 1)[0]
        exported[name] = full
    if set(exported) != names:
        errors.append({'missing': sorted(names - set(exported)),
                       'unexpected': sorted(set(exported) - names)})
    wrong_versions = [full for full in exported.values() if not full.endswith('@@LOGGER_0.9')]
    if wrong_versions:
        errors.append({'wrong_default_symbol_versions': wrong_versions})
    sonames = re.findall(r'\(SONAME\).*?\[(.*?)\]', dynamic)
    if sonames != ['liblogger.so.0']:
        errors.append({'soname': sonames})
    if re.search(r'\((?:RPATH|RUNPATH|TEXTREL)\)', dynamic):
        errors.append('Production ELF contains RPATH/RUNPATH/TEXTREL')
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
    banned = [n for n in needed if n.startswith(('libcrypto.', 'libssl.', 'libasan.', 'libubsan.', 'libtsan.'))]
    if banned:
        errors.append({'forbidden_dependencies': banned})
    glibc = sorted({tuple(map(int, n.split('.'))) for n in re.findall(r'Name: GLIBC_([0-9.]+)', version_info)})
    ceiling = tuple(map(int, max_glibc.split('.'))) if max_glibc else None
    if ceiling and not glibc:
        errors.append('GLIBC ceiling requested but no GLIBC version requirement was found')
    elif ceiling and glibc[-1] > ceiling:
        errors.append({'glibc_required': '.'.join(map(str, glibc[-1])), 'ceiling': max_glibc})
    if 'GLIBC_PRIVATE' in version_info:
        errors.append('GLIBC_PRIVATE dependency')
    def field(label):
        m = re.search(r'^\s*' + re.escape(label) + r':\s*(.*)$', header, re.M)
        return m.group(1) if m else None
    if field('Type') is None or not field('Type').startswith('DYN '):
        errors.append({'elf_type': field('Type'), 'expected': 'DYN'})
    if machine and field('Machine') != machine:
        errors.append({'machine': field('Machine'), 'expected': machine})
    if elf_class and field('Class') != elf_class:
        errors.append({'class': field('Class'), 'expected': elf_class})
    return {'artifact': str(artifact), 'passed': not errors, 'errors': errors,
            'exports': exported, 'soname': sonames, 'needed': needed,
            'machine': field('Machine'), 'class': field('Class'), 'data': field('Data'),
            'required_glibc_versions': ['.'.join(map(str, t)) for t in glibc],
            'glibc_ceiling': max_glibc, 'expected_machine': machine,
            'expected_class': elf_class, 'kernel_compatibility': 'not established by ELF inspection'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact', type=Path)
    p.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[1] / 'cmake/logger.symbols')
    p.add_argument('--legacy-fork', action='store_true')
    p.add_argument('--max-glibc', help='Optional deployment baseline ceiling, e.g. 2.25; FAIL rather than rewrite version requirements')
    p.add_argument('--machine', help='Exact readelf Machine value for the target, e.g. Advanced Micro Devices X86-64')
    p.add_argument('--elf-class', choices=('ELF32', 'ELF64'), help='Required target ELF class')
    a = p.parse_args()
    if a.max_glibc and not re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', a.max_glibc):
        p.error('--max-glibc must be a dotted numeric version, e.g. 2.25')
    try:
        report = inspect(a.artifact.resolve(strict=True), a.manifest.resolve(strict=True),
                         a.legacy_fork, a.max_glibc, a.machine, a.elf_class)
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print(json.dumps({'passed': False, 'error': str(e)}, indent=2))
        return 2
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
