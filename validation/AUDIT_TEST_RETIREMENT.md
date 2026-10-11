# Audit support retirement: retained ordinary Logger coverage

Baseline: `cea39bb9a3d711de994e8dab003976e6c3f6abf4`, identical source tree to
merged main `2e66c96ebfffd867c12c271e7782e2af5c7bfd50`.
This batch retires historical support only. It does not change VERSION,
release-publish, release authorization, the current public ABI, or the eight-point
process/VM crash oracle. Existing v1 manifests remain unchanged historical records;
there is no ongoing v1 compatibility gate.

## Mixed-test scenario mapping

| Previous test | Ordinary coverage retained/migrated | Explicitly retired |
| --- | --- | --- |
| `test_faults.c` | `file_write`, `file_fsync`, strict sync errors | Audit `state_write`, `state_fsync`, `state_rename` |
| `test_lock_failure.c` | `normal`, `emit`, `emit-unlock`, `queue-notify`, `queue-signal`, `force-wake` (6) | Audit operation lock/unlock (2) |
| `test_process_fork.c` | 17 Logger/Console/context cases below; child poison pointers, pre-lock/pre-I/O rejection, sticky atfork registration errors | `audit-only`, `verify-only`, `register-audit`, `register-verify` (21 -> 17); private removed offset API probe |
| `test_explicit_scope.c` | 88 non-Audit cases: 14 cancellation, 7 Console cancellation, 28 write-origin reentry, 6 x 5 other-origin reentry, 7 contracts, 2 global-worker reentry | `audit`/`audit-status` entries only |
| `test_fault_isolation.c` | Each production/support target retains `file_write`, `file_fsync`, `file_rename`, `short-write`, `syslog-path`; adds the 4 ordinary rotation environment hooks | `state_*`, Audit tail `file_ftruncate`, and 4 Audit checkpoint/crash cases |
| `test_lockdep.c` | Six ordinary lock classes, all Global/instance rules, trylock, real ordering, thread-local and condition wait checks | Two Audit classes and their two permitted-order examples |
| `test_audit_format.c` -> `test_logger_format.c` | Nine scenarios now call real ordinary Logger strict/ordinary paths; truncation, cache, time fallback, no partial writes and recovery | Audit chain/checkpoint/status assertions |
| `run_audit_format.sh` -> `run_logger_format.sh` | Both probes link actual production `liblogger.a`; all checked-formatter boundaries remain | Dependency on historical regression archive |
| `v1_abi_contract.{c,cpp}` -> `current_abi_contract.{c,cpp}` | Current Logger/Console layout/default/function-type assertions unchanged | Misleading historical target/file name and standalone v1 symbol snapshot gate |

The exact 17 process scenarios are `global`, `explicit`, `console-only`,
`context-only`, `emit-held`, `progress-held`, `console-held`, `reader-held`,
`registration-window`, `earlier-handler`, `after-shutdown`, `bypass-handler`,
`prefork`, `register-global`, `register-explicit`, `register-console`,
`register-context`. An explicit whitelist rejects unknown/retired scenario names.

The nine formatter scenarios are `normal`, `cache-hit`, `localtime`, `date`,
`zone`, `bad-date`, `bad-zone`, `cached-bad`, `truncation`. Strict failures must
leave the entire existing file unchanged; successful records match complete
expected bytes. Ordinary fallback remains visible after conversion failure.
Metadata and message overflow are rejected by strict output and marked by
ordinary output. The checked-formatter probe retains every old capacity,
strict/ordinary parity, `vsnprintf`/`__vsnprintf_chk` failure and nanosecond assertion.
Both production formatter probes are now registered with Xmake for static suites,
in addition to the independent CI runner.

Production isolation now links `logger`, rather than `logger_regression_support`,
in both static and shared configurations. Executable `connect` interposition uses
baseline-supported `--export-dynamic`, and short-write is measured through public
file syscall counters. Test hooks remain in the uninstalled support archive.
The four environment crash hooks are `file_after_archive_rename`,
`file_after_archive_dirsync`, `file_after_active_open`, `file_after_active_dirsync`.
The other four crash-matrix boundaries remain fixture acknowledgments/real fsync
wrappers, not invented environment hooks.

`file_legacy_probe` is an ordinary black-box defect regression, not an old ABI
adapter. All seven owner/reopen/collision/cwd/symlink/hardlink/global-repeat cases
remain. Existing rollback, common cleanup, scope, process census, destroy receipt,
flush/release-proof, message truncation and constructor tests remain.

## Deleted code and retained ownership

Deleted: Audit source/header files, public `audit.h`, pure Audit/crypto tests and
fixtures, digest helper/tool, and cross-version crypto runner. The support archives
remain, but compile only Logger sources (plus test-only faults where appropriate).

The only product consumers of `logger_create_reserved_file`, `logger_file_offset`
and `logger_file_offset_get` were Audit. These private paths are removed.
`logger_file_init` still reserves the directory/lease before opening the active
file: reserve/open-reserved helpers are now file-local. No ordinary constructor
rollback, file lease cleanup, final-release receipt or census algorithm is removed.
The three ordinary fault points remain; Audit state/truncate points and unused
`before_audit_fsync` are removed. Existing ordinary rotation hooks are unchanged.

The production artifact gate retains all prior exclusions and additionally rejects
Audit-named archive members even if they contain no Audit symbol. Static/shared
negative fixtures prove Audit symbols and test hooks are rejected. No installed
header, package component, SONAME, symbol version or current 47-symbol allowlist is
relaxed. Config version/size/nonzero-tail failure tests and relocated C/C++/PIC/
pkg-config consumers remain current gates.

## Validation and limits

Local commands and outcomes are recorded below after the final run. A local
failure caused by sandboxed AF_UNIX creation or sanitizer runtime restrictions is
not a pass and is not bypassed. Exact-commit remote CI remains required, including
Ubuntu 20.04 GCC/Clang, Python 3.8, sanitizers, current/minimum kernel, full process
and all 16 QEMU filesystem/cut-point cells. Prior baseline QEMU success does not
substitute for final-commit evidence.

A separate pre-existing test gap was discovered but deliberately not changed:
`explicit_scope` registers cancellation `file-metrics`, but its caller falls
through to ordinary logging instead of calling `logger_get_file_metrics`.
This batch preserves that code; a dedicated follow-up must fix and prove it.

### Local run (2026-10-11, GCC 14.2 / Python 3.13 / sandbox)

- Final release static suite: 410 passed / 470, 60 failed. Shared: 406 / 485,
  79 failed. The failed lists contain only AF_UNIX-dependent Syslog cases;
  `socket` is denied with EPERM. These are failures, not skips or a full pass.
- Per-scenario retained coverage in both release configurations: faults 2/2,
  lock failures 6/6, process fork 17/17, ordinary file legacy probes 7/7,
  explicit scope 87/88 (only Syslog metrics blocked). Static production formatter
  9/9 and checked formatter 1/1 passed, as did its independent production runner.
- Production/support hook isolation each passed 8/9 per release configuration;
  only Syslog path observation was blocked before connect by socket EPERM.
- Each release configuration passed the exact 8-point process group; independent
  crash verifier passed 463 acceptance/rejection fixtures and six matrix-log
  checks. The VM guest built; no new local QEMU run is claimed.
- Static/shared current C and C++ semantic ABI executables passed. Relocated
  installs passed real C/C++11/PIC/pkg-config consumers, strict version/component
  negatives and isolation; the shared artifact passed 47-symbol LOGGER_2.0 /
  SONAME 2 checks. Both XPack archives and checksum checks passed.
- Worker startup (9 scenarios) and worker failure (13 scenarios) production
  runners passed. Mutex policy and eight artifact-exclusion fixture methods
  passed. Modified Python files parse with Python 3.8 grammar; that is not a
  substitute for real Python 3.8 execution in the retained Ubuntu 20.04 CI.
- Full ASan+UBSan suite compiled; 0/470 runtime tests passed. LeakSanitizer reports
  its fatal ptrace limitation (427 per-case stderr logs contain that message),
  and the independent formatter runner stops at the same runtime limitation.
  Leak detection was enabled for this original run. This is not an ASan/UBSan
  acceptance result.
- A separate authorized partial run used `ASAN_OPTIONS=detect_leaks=0`
  and `UBSAN_OPTIONS=halt_on_error=1`: ASan+UBSan passed 410/470, with the
  same 60 AF_UNIX/EPERM-dependent failures and no ASan/UBSan diagnostic.
  Independent production formatter (9 + checked), worker startup (9), and
  worker failure (13) runners all passed with those settings. This only validates
  address/undefined behavior checks; LeakSanitizer remains unvalidated and its
  original 0/470 failure is retained above. No system permission was changed.
- Full TSan suite compiled and ran: 410/470 passed, the same 60 socket-dependent
  failures; no TSan race diagnostic appeared. Both sanitizer production archives
  passed the unchanged hook/crypto exclusion boundary.
- After packaging, manual runners correctly rejected the duplicate archive in
  XPack staging. Moving that generated staging cache outside the build directory
  restored the single-artifact invariant, and all three runners passed again.
- First feeding the complete-suite log to the crash-only matrix checker correctly
  failed on extra case names. A dedicated process-crash run was then checked and
  passed in both configurations; the checker and oracle were not weakened.

Logs and precise working-tree patch hashes accompany the review handoff. Local
working-tree validation precedes a final commit; only exact-commit remote CI can
close the remaining baseline/sanitizer/socket/QEMU acceptance requirements.
