#!/usr/bin/env python3
"""检查 Linux 最终消费端 ELF，包括链接 liblogger.a 的消费端。

静态归档没有最终动态 ABI。本工具检查最终 ELF 的直接 GLIBC 版本需求，
不认证传递依赖库、内核或任意消费端工具链。
发布 CI 还会在基线环境中构建并运行消费端。
"""
# JSON 报告字段、状态值与诊断值保留兼容拼写。
import json
import os
from pathlib import Path
import re
import subprocess


def run(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          env=dict(os.environ, LC_ALL='C')).stdout


def inspect(artifact, max_glibc, allow_no_glibc_for=None):
    with artifact.open('rb') as stream:
        if stream.read(8) == b'!<arch>\n':
            raise ValueError('Inspect the final linked ELF, not a static archive')
    header = run('readelf', '-hW', str(artifact))
    versions = run('readelf', '--version-info', '-W', str(artifact))
    errors = []

    def field(label):
        match = re.search(r'^\s*' + re.escape(label) + r':\s*(.*)$', header, re.M)
        return match.group(1) if match else ''

    if field('Type').split(' ', 1)[0] not in ('EXEC', 'DYN'):
        errors.append('Expected a final executable or shared ELF')
    if field('Machine') != 'Advanced Micro Devices X86-64' or field('Class') != 'ELF64':
        errors.append('Expected Linux x86_64 / ELF64 consumer')
    requirements = set(re.findall(r'Name: (GLIBC_\S+)', versions))
    numeric = {name for name in requirements
               if re.fullmatch(r'GLIBC_[0-9]+(?:\.[0-9]+)+', name)}
    glibc = sorted({tuple(map(int, name[len('GLIBC_'):].split('.'))) for name in numeric})
    unknown = requirements - numeric - {'GLIBC_PRIVATE'}
    if unknown:
        errors.append({'unrecognized_glibc_requirements': sorted(unknown)})
    if not glibc:
        # 共享消费端模块可能仅调用 Logger，所有 libc 调用都位于单独检查的
        # liblogger DSO 内。不得将此豁免用于嵌入静态归档的模块，
        # 也不得用于链接 libc 的可执行文件。
        needed = []
        if allow_no_glibc_for and field('Type').startswith('DYN '):
            dynamic = run('readelf', '-dW', str(artifact))
            needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
        if not allow_no_glibc_for or needed != [allow_no_glibc_for]:
            errors.append('No GLIBC version requirement found; baseline not established')
    elif glibc[-1] > tuple(map(int, max_glibc.split('.'))):
        errors.append({'glibc_required': '.'.join(map(str, glibc[-1])),
                       'ceiling': max_glibc})
    if 'GLIBC_PRIVATE' in versions:
        errors.append('GLIBC_PRIVATE dependency')
    return {'artifact': str(artifact), 'passed': not errors, 'errors': errors,
            'required_glibc_versions': ['.'.join(map(str, version)) for version in glibc],
            'glibc_ceiling': max_glibc, 'machine': field('Machine'), 'class': field('Class'),
            'allow_no_glibc_for': allow_no_glibc_for,
            'scope': 'direct final-ELF requirements; run on target userland too'}


def main():
    from tool_i18n import parser_for
    parser, _ = parser_for('Check final Linux consumer ELFs, including consumers linked to liblogger.a.\n\nAn archive has no final dynamic ABI. This checks direct GLIBC version needs of\nthe final ELF; it does not certify transitive libraries, kernels or arbitrary\nconsumer toolchains. Release CI also builds and runs consumers on the baseline.')
    parser.add_argument('artifact', type=Path)
    parser.add_argument('--max-glibc', required=True)
    parser.add_argument('--allow-no-glibc-for', metavar='SONAME',
                        help=_('Allow no direct GLIBC only in a shared module whose sole dependency is this separately checked Logger SONAME'))
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', args.max_glibc):
        parser.error(_('--max-glibc must be a dotted numeric version, e.g. 2.31'))
    try:
        report = inspect(args.artifact.resolve(strict=True), args.max_glibc, args.allow_no_glibc_for)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(json.dumps({'passed': False, 'error': str(error)}, indent=2))
        return 2
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
