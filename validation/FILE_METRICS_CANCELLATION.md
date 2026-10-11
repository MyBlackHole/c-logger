# File-metrics cancellation test wiring

## Scope

Discovery baseline: `2e66c96ebfffd867c12c271e7782e2af5c7bfd50`.
Final integration and verification baseline, after Audit retirement:
`1774e61e8c18b605f481cd1078f8476dd6be078a`.
This is a test-only repair. No production source, public API, ABI, cancellation
policy, or ownership/lifetime contract changes.

The registered `explicit_cancel_file-metrics` case armed `P_LOCK` but its caller
had no `file-metrics` branch. It fell through to `LOGGER_INFO`, which also takes
`emit_mu`; the old test passed without calling `logger_get_file_metrics()`.
The existing nested `file-metrics` reentry case exercises a different contract
and does not establish cancellation coverage.

The repaired caller invokes the real getter. A link-time getter wrapper counts
entries and marks its active stack in thread-local test state. The existing
mutex wrapper counts successful acquisitions of the selected instance's
`emit_mu` while that getter is active. The paused file-metrics case requires
exactly one getter entry and exactly one such lock acquisition, both before
cancellation and after joining the canceled thread.

All prior cancellation assertions remain: cancellation is disabled at the real
critical section, cleanup has not run while the operation is paused, the joined
thread is canceled, same-thread application cleanup can log and flush again,
and final flush/destruction leaves zero registered objects and the original
open-fd count. Deferred cancellation can be delivered at the caller's existing
`pthread_testcancel()` after the API restores the cancellation state; this test
does not introduce a stronger cancellation-delivery contract.

## Same-fixture negative control

First build the strengthened fixture with the baseline caller, omitting only
the new three-line `file-metrics` dispatch branch. Run:

```sh
xmake test -j1 'explicit_scope_regression/explicit_cancel_file-metrics'
```

The test executable exits 1 (Xmake exits 255), with `console.out` containing:

```text
file-metrics getter calls: 0 (expected 1)
... calls == 1 (errno=2)
```

The getter wrapper, lock wrapper, precise assertions, and Xmake wrap list are
identical between this negative control and the repaired fixture. Restoring
only the caller branch makes the file-metrics cancellation case pass. This
reproduces a false-positive test, not a getter implementation failure.

## Verification

Xmake 3.1.1, GCC 14.2, Linux x86_64; test configuration uses
`build_tests=y`, `build_private_tests=n`, and `build_regression_tests=y`.

| Configuration | Explicit/Console scope target | Production core | Related tests |
|---|---|---|---|
| Release static | 87 passed, 1 environment-blocked | 15/15 | 128/128 |
| Release shared | 87 passed, 1 environment-blocked | 15/15 | 122/122 |
| Debug ASan + UBSan static | 87 passed, 1 environment-blocked | 15/15 | 128/128 |
| Debug TSan static | 87 passed, 1 environment-blocked | 15/15 | 128/128 |

All 88 non-Audit scope registrations remain unchanged. The sole failed scope
case in every configuration is the preserved
`explicit_cancel_syslog-metrics`: this executor denies its AF_UNIX socket
creation with `EPERM`. It was not skipped, stubbed, or rerun with escalated
permissions. The repaired file-metrics case passes in all four configurations, plus 20
additional consecutive executions per configuration (80/80).

The related set comprises `file_backend_regression`, `file_global_regression`,
`global_cancel_regression`, `global_lifecycle_regression`,
`constructor_rollback_regression`, `destroy_status_regression`,
`flush_release_proof_production_test`, and `flush_release_proof_regression`.
The six production flush cases use the real production archive; other related
regressions use the existing white-box archive. The shared related set excludes
only the six static-only production flush cases. The production core is the
15 existing tests registered in the `logger_tests` list in `xmake.lua`.

The scope target always links `logger_regression_support`, including under the
shared configuration. It is not DSO-internal `--wrap` interception. Shared core
tests really link the production `liblogger.so.2`. Isolation checks pass for
all four production artifacts.

ASan uses `ASAN_OPTIONS=detect_leaks=0`; LSan is not verified in this restricted
executor. UBSan and TSan use `halt_on_error=1`. No full-suite, remote-CI,
minimum-kernel, physical-power-loss, or deployment qualification is claimed.
