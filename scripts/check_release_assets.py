#!/usr/bin/env python3
"""上传或发布前，核验 shared/static 发布包资产的精确集合。"""
# verified 校验和记录保持原样，便于既有自动化证据消费。
import hashlib
import re
from pathlib import Path


def verify(directory, version, kinds, _=lambda message: message):
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){2}', version):
        raise ValueError(_('version must be MAJOR.MINOR.PATCH'))
    for kind in kinds:
        matches = sorted(directory.glob('prod-c-logger-{}-*-{}.tar.gz'.format(version, kind)))
        if len(matches) != 1:
            raise ValueError(_('Expected exactly one {kind} package, found {count}').format(kind=kind, count=len(matches)))
        package = matches[0]
        checksum = package.with_name(package.name + '.sha256')
        line = checksum.read_text(encoding='ascii').strip()
        match = re.fullmatch(r'([0-9a-f]{64})  ([^/\\\s]+)', line)
        if not match or match.group(2) != package.name:
            raise ValueError(_('Invalid checksum record for {package}').format(package=package.name))
        digest = hashlib.sha256()
        with package.open('rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(chunk)
        if digest.hexdigest() != match.group(1):
            raise ValueError(_('SHA-256 mismatch for {package}').format(package=package.name))
        print('verified {} {}'.format(package.name, digest.hexdigest()))


def main():
    from tool_i18n import parser_for
    parser, _ = parser_for('Verify the exact shared/static release package assets before upload or publication.')
    parser.add_argument('--directory', type=Path, default=Path('.'))
    parser.add_argument('--version', required=True)
    parser.add_argument('--kind', choices=('shared', 'static'), action='append',
                        help=_('Check one or both package kinds; default checks both'))
    args = parser.parse_args()
    try:
        verify(args.directory, args.version, args.kind or ('shared', 'static'), _)
    except (OSError, ValueError) as error:
        parser.exit(1, _('Release asset verification failed: {error}\n').format(error=error))


if __name__ == '__main__':
    main()
