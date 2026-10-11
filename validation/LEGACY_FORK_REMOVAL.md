# Legacy fork helper removal

Validated on 2026-10-11, Linux x86_64, GCC 14.2.0, Xmake 3.1.1,
CMake 3.31.6. This batch is based on `ae2671e2f3f39a705672b90cf8683c34a814d324`.

## Removed and retained boundaries

Removed the optional `logger_fork_reinit` implementation, compatibility header,
example, dedicated 23-case regression, Xmake option/target/install/export branches,
legacy script profile, legacy CI job, and packaging/ABI-checker bypass flags.
The old helper contract is retained only as an uninstalled `HISTORY_` document.

The default production behavior is preserved: PID identity, `pthread_atfork`
child invalidation, early ECHILD rejection, and the process object census remain.
No child identity reset or inherited mutex repair is introduced. Normal raw-fork,
constructor rollback, destroy, flush, global lifecycle, cancellation and resource
cleanup tests remain registered. `VERSION`, current public symbol manifests,
Audit/crash/QEMU implementation and release-publish policy are unchanged.

Production isolation now always rejects process-creation helpers and procfs
thread scanning. The symbol check inspects the `nm` symbol field instead of the
artifact path, so directories named `build-fork` do not cause false positives.
Symbol versions and compiler clone suffixes do not bypass the check.

The new packaging checker tests cover 29 static/shared fixture checks in six
unittest methods: valid fork-named paths and atfork defenses; every banned symbol;
a defined legacy helper; compiler clones; `/proc/self/task`; and the removed
`--legacy-fork` bypass. Fixtures are compiled and inspected, never executed.
They run in the existing parity production-artifact job.

## Local results

| Configuration/check | Result |
|---|---|
| Release static, production core and related regressions | 240 passed / 241 selected |
| Release shared, production core and related regressions | 234 passed / 235 selected |
| Debug static ASan + UBSan, same selection | 240 passed / 241 selected |
| Debug static TSan, same selection | 240 passed / 241 selected |
| Additional real DSO global async/sync/init-race and dlclose | 4/4 passed |
| Release static/shared and both sanitizer production isolation | Passed |
| Release shared SONAME 2, LOGGER_2.0, current 47-symbol manifest | Passed |
| Static and shared staged install / relocation / consumers | Passed |
| Checker fixtures | 6/6 methods, 29/29 fixture checks passed |
| Removed Xmake option, script profile and ABI-checker flag | Correctly rejected |
| Python syntax, shell syntax, workflow YAML, diff whitespace | Passed |

All four selected-suite runs have exactly the same environmental failure:
`explicit_scope_regression/explicit_cancel_syslog-metrics` fails at
`socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)` with EPERM before exercising the
library. A diagnostic retry and one permission-reviewed execution attempt
confirmed the same failure; no further permission workaround was attempted.
This case remains enabled and must be verified by GitHub CI. These local suite
runs returned failure and are not represented as complete passes.

All 21 `process_fork_regression` scenarios passed in all four configurations,
including PID-only bypass-handler defense, registration windows, early child
handlers, held inherited locks, and post-shutdown rejection. The shared
configuration's white-box regressions link the same-source static support
archive. Production-linked core tests and the extra DSO tests use the actual
shared library; white-box results are not DSO-interception claims.

ASan used `ASAN_OPTIONS=detect_leaks=0` because this executor restricts the leak
scanner. UBSan and TSan used `halt_on_error=1`. No sanitizer finding was reported
in the exercised cases; this does not establish LSan coverage.

Both install checks retain relocated prefixes with spaces, public-header-only
installation, C and C++11 consumers, PIC SDK plugin loading, independent
pkg-config consumption, exact-version rejection, shared/static component
selection, unknown/retired `legacy_fork` component rejection, current C/C++
layouts/defaults, production isolation, and shared versioned-symbol loading.
The deliberately impossible GLIBC 2.0 ceiling still fails closed.

## Reproduce focused runtime checks

Export the repository `VERSION` and configure one mode at a time:

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -c -y -m release -o build-static --build_shared=n \
  --build_tests=y --build_private_tests=y --build_regression_tests=y
```

For shared, select `--build_shared=y` and a separate output directory. For
ASan/UBSan or TSan, select debug/static and respectively
`--policies=build.sanitizer.address,build.sanitizer.undefined` or
`--policies=build.sanitizer.thread`.

Run the same focused selection after each configuration:

```sh
xmake test -j1 \
  'test_logger/*' 'rotation_test/*' 'console_test/*' 'format_test/*' \
  'context_test/*' 'permissions_test/*' 'fork_test/*' 'fork_guard_test/*' \
  'lifecycle_test/*' 'concurrency_stress_test/*' 'api_contract_test/*' \
  'config_abi_test/*' 'redaction_test/*' 'reinit_test/*' 'multi_instance_test/*' \
  'stderr_sigpipe_production_test/*' 'console_sigpipe_production_test/*' \
  'flush_release_proof_production_test/*' 'process_fork_regression/*' \
  'destroy_status_regression/*' 'constructor_rollback_regression/*' \
  'resource_cleanup_regression/*' 'global_lifecycle_regression/*' \
  'global_cancel_regression/*' 'explicit_scope_regression/*' \
  'host_ownership_regression/*' 'flush_release_proof_regression/*'
```

The six `flush_release_proof_production_test` cases are static-only. With the
shared configuration, additionally run:

```sh
xmake test -j1 'global_shared_regression/*' 'liblogger_dlclose_test/*'
```

## Reproduce artifact and installation checks

```sh
python3 tests/packaging/test_production_artifact.py -v
python3 scripts/check_production_artifact.py <production-artifact>
python3 scripts/check_release_abi.py <release-shared-artifact> \
  --machine 'Advanced Micro Devices X86-64' --elf-class ELF64
python3 tests/packaging/check_install.py \
  --source . --build <build-directory> --artifact <production-artifact> \
  --kind <static-or-shared> --cc gcc --cxx g++
```

The installed checker rejects the legacy macro in both CMake/pkg-config metadata
and consumer compile commands. Only the four current public headers may be
installed; the fork compatibility header is not accepted.

## Not established by this batch

The full unfiltered historical suite, XPack packaging, QEMU/raw ext4/XFS powercut,
physical storage qualification, remote GitHub CI, and LSan were not run here.
No Audit/crash/QEMU tests or fixtures were removed or relabeled by this batch.
The AF_UNIX-dependent case and final-head CI remain required before merge.

## Final integration with merged truncation/flush fixes

The cleanup was then rebased onto merged main
`1b31c43d2fd74e3a77d40eba1f9c3047d995e01c`. The saved cleanup patch applied
cleanly with three-way context, without manual conflict resolution. The message
truncation production target, production/static flush-release-proof target, and
white-box flush-release-proof target all remain registered. The merged
truncation implementation, public header contract and API description remain.

All four modes were reconfigured and rebuilt against that final combination.
The original selection was expanded by all seven
`message_truncation_production_test` scenarios plus `host_format_regression`;
shared also included the four real DSO cases in the same run:

| Final integrated configuration | Result |
|---|---|
| Release static | 248 passed / 249 selected |
| Release shared, including real DSO tests | 246 passed / 247 selected |
| Debug static ASan + UBSan, LSan disabled | 248 passed / 249 selected |
| Debug static TSan | 248 passed / 249 selected |
| Both refreshed release install/relocation/consumer checks | Passed |
| All four refreshed production-isolation checks | Passed |
| Refreshed shared ABI / SONAME / versioned symbol checks | Passed |
| Fork-exclusion checker fixtures | 6/6 methods, 29/29 checks passed |

Every final run retains exactly one failure: the same AF_UNIX EPERM case.
Verbose test logs preserve `receiver >= 0 (errno=1)` for each configuration.
No new sanitizer finding or runtime failure appeared. Each configuration passed
all 21 raw-fork scenarios, and the new seven truncation and one format-boundary
cases. Final-head GitHub CI must still exercise the AF_UNIX-dependent case.

Append these selectors to the focused command above to reproduce the final
combined run (the two real DSO targets exist only for shared):

```sh
'message_truncation_production_test/*' 'host_format_regression/*' \
'global_shared_regression/*' 'liblogger_dlclose_test/*'
```
