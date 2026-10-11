#!/usr/bin/env python3
"""检查生产归档是否意外包含测试钩子，包括 Debug/-O0 构建。
接受静态归档或共享 ELF 对象，检查测试钩子及禁止的外部密码学 API 和依赖。
需要 nm、ar、strings 和 readelf；此检查不证明任意宿主依赖的性质。
"""
# JSON 报告字段、状态值与诊断值保留兼容拼写。
from tool_i18n import parser_for
import json
import re
import subprocess
from pathlib import Path

parser, _ = parser_for('Check a production archive, even a Debug/-O0 build, for accidental test hooks.\nAccepts static archives or shared ELF objects. Checks test hooks and forbidden external crypto APIs/dependencies. Requires nm, ar, strings and readelf. Not a proof about arbitrary host dependencies.')
parser.add_argument('archive', type=Path)
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
             'before_audit_fsync', 'after_audit_fsync', 'after_checkpoint_commit', 'before_state_rename',
             'after_state_rename', 'file_after_archive_rename', 'file_after_archive_dirsync',
             'file_after_active_open', 'file_after_active_dirsync']
failures = [v for v in forbidden if v in strings]
if 'logger_fault.c.o' in members:
    failures.append('logger_fault.c.o')
for member in members.splitlines():
    if Path(member).name.startswith(('audit.', 'audit_')):
        failures.append('retired Audit source member: ' + member)
for line in symbols.splitlines():
    parts = line.split()
    if parts and parts[-1].split('@', 1)[0].startswith('audit_'):
        failures.append('retired Audit symbol: ' + line)
    if any(name in line for name in ['logger_fault_should_fail', 'logger_fault_short_write', 'logger_fault_crash_if_requested']):
        failures.append(line)
if '/proc/self/task' in strings:
    failures.append('/proc/self/task')
process_helpers = {'fork', 'vfork', 'posix_spawn', 'posix_spawnp',
                   'logger_fork_reinit', 'logger_process_thread_count',
                   'logger_process_arm_clean_fork', 'logger_process_finish_clean_fork',
                   'logger_global_stop_for_clean_fork', 'logger_global_lifetime_broken'}
for line in symbols.splitlines():
    parts = line.split()
    # nm -A 在每个符号前添加产物路径。排除进程辅助函数时应检查符号，
    # 不应匹配 build-fork 等目录名。
    # 带编译器克隆后缀（例如 .constprop.0）的符号也必须禁止。
    if parts and parts[-1].split('@', 1)[0].split('.', 1)[0] in process_helpers:
        failures.append(line)
# 即使在调试构建中，本库也不得引入任何密码学后端。
# 检查全部归档符号，确保意外静态链接的 API 对象同样被拒绝。
# 上文已独立检查并拒绝 Audit 符号。
crypto_prefixes = ('EVP_', 'OPENSSL_', 'CRYPTO_', 'OSSL_', 'SSL_', 'SHA256_', 'SM3_')
for line in symbols.splitlines():
    parts = line.split()
    if parts and parts[-1].split('@', 1)[0].startswith(crypto_prefixes):
        failures.append('external crypto symbol: ' + line)
# 已停用的 SM3 实现不得作为无用代码残留在 Debug 产物中。
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
          'members':members.splitlines(), 'needed':needed, 'crypto_implementation':'none'}
print(json.dumps(report, indent=2))
raise SystemExit(1 if failures else 0)
