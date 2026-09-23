#!/usr/bin/env python3
"""Check a production archive, even a Debug/-O0 build, for accidental test hooks.
Accepts static archives or shared ELF objects. Checks test hooks and forbidden external crypto APIs/dependencies. Requires nm, ar, strings and readelf. Not a proof about arbitrary host dependencies.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('archive', type=Path)
parser.add_argument('--legacy-fork', action='store_true', help='allow the opt-in process-creation helper')
args = parser.parse_args()
archive = args.archive.resolve(strict=True)

def output(command):
    return subprocess.run(command + [str(archive)], check=True, capture_output=True, text=True).stdout

with archive.open('rb') as f:
    is_archive = f.read(8) == b'!<arch>\n'
members = output(['ar', 't']) if is_archive else ''
symbols = output(['nm', '-A'])
strings = output(['strings'])
forbidden = ['LOGGER_FAULT_POINT', 'LOGGER_FAULT_AFTER', 'LOGGER_FAULT_ERRNO',
             'LOGGER_FAULT_SHORT_WRITE', 'LOGGER_CRASH_POINT', 'LOGGER_SYSLOG_PATH',
             'after_audit_fsync', 'after_checkpoint_commit', 'before_state_rename',
             'after_state_rename', 'file_after_archive_rename', 'file_after_archive_dirsync',
             'file_after_active_open', 'file_after_active_dirsync']
failures = [v for v in forbidden if v in strings]
if 'logger_fault.c.o' in members:
    failures.append('logger_fault.c.o')
for line in symbols.splitlines():
    if any(name in line for name in ['logger_fault_should_fail', 'logger_fault_short_write', 'logger_fault_crash_if_requested']):
        failures.append(line)
if not args.legacy_fork:
    if '/proc/self/task' in strings:
        failures.append('/proc/self/task')
    for line in symbols.splitlines():
        if re.search(r'\b(fork|vfork|posix_spawn|logger_fork_reinit|logger_process_thread_count|logger_process_arm_clean_fork|logger_process_finish_clean_fork)\b', line):
            failures.append(line)
# No crypto backend may be brought in by this library, even in a debug build.
# Inspect all archive symbols so accidentally statically linked API objects are
# rejected as well. Exported audit_crypto_* names are not external crypto APIs.
crypto_prefixes = ('EVP_', 'OPENSSL_', 'CRYPTO_', 'OSSL_', 'SSL_', 'SHA256_', 'SM3_')
for line in symbols.splitlines():
    parts = line.split()
    if parts and parts[-1].split('@', 1)[0].startswith(crypto_prefixes):
        failures.append('external crypto symbol: ' + line)
# Retired SM3 implementation must not remain as dead code in Debug artifacts.
for line in symbols.splitlines():
    if re.search(r"\b(?:builtin_sm3|audit_sm3|sm3_[A-Za-z0-9_]+)\b", line):
        failures.append('retired digest symbol: ' + line)
if 'SM3/builtin' in strings:
    failures.append('retired digest tag: SM3/builtin')
needed = []
if not is_archive:
    dynamic = output(['readelf', '-d'])
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
    for name in needed:
        if name.startswith(('libcrypto.', 'libssl.')):
            failures.append('external crypto dependency: ' + name)
report = {'artifact_type':'archive' if is_archive else 'ELF', 'archive':str(archive), 'passed':not failures, 'failures':failures,
          'members':members.splitlines(), 'needed':needed, 'crypto_implementation':'builtin-sha256-only'}
print(json.dumps(report, indent=2))
raise SystemExit(1 if failures else 0)
