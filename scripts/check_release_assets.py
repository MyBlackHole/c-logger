#!/usr/bin/env python3
"""Verify the exact shared/static CPack assets before upload or publication."""
import argparse
import hashlib
import re
from pathlib import Path


def verify(directory, version, kinds):
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){2}', version):
        raise ValueError('version must be MAJOR.MINOR.PATCH')
    for kind in kinds:
        matches = sorted(directory.glob('prod-c-logger-{}-*-{}.tar.gz'.format(version, kind)))
        if len(matches) != 1:
            raise ValueError('expected exactly one {} package, found {}'.format(kind, len(matches)))
        package = matches[0]
        checksum = package.with_name(package.name + '.sha256')
        line = checksum.read_text(encoding='ascii').strip()
        match = re.fullmatch(r'([0-9a-f]{64})  ([^/\\\s]+)', line)
        if not match or match.group(2) != package.name:
            raise ValueError('invalid checksum record for {}'.format(package.name))
        digest = hashlib.sha256()
        with package.open('rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(chunk)
        if digest.hexdigest() != match.group(1):
            raise ValueError('SHA-256 mismatch for {}'.format(package.name))
        print('verified {} {}'.format(package.name, digest.hexdigest()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('.'))
    parser.add_argument('--version', required=True)
    parser.add_argument('--kind', choices=('shared', 'static'), action='append',
                        help='Check one or both package kinds; default checks both')
    args = parser.parse_args()
    try:
        verify(args.directory, args.version, args.kind or ('shared', 'static'))
    except (OSError, ValueError) as error:
        parser.exit(1, 'release asset verification failed: {}\n'.format(error))


if __name__ == '__main__':
    main()
