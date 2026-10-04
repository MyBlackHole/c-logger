# Audit Recovery Capacity Validation

Production v1 hardening tracks the current Audit retained-history recovery limit before deciding whether the
O(N²) segment-connection algorithm needs redesign.

## Scope

The dedicated `audit-recovery-capacity` workflow builds a non-installed regression executable and creates:

- 4096 valid archive files, matching the production archive-name grammar;
- one valid active Audit file;
- one chained SHA-256 record per archive plus one active record.

The benchmark calls the real `audit_recover_set()` implementation. Setup time is reported separately and is
not counted as recovery time.

## Measurements

The workflow records:

- valid 4096-archive recovery wall time;
- missing-middle recovery wall time and required `EBADMSG`;
- corrupt-middle recovery wall time and required `EBADMSG`;
- truncated-middle recovery wall time and required `EBADMSG`;
- process `ru_maxrss`;
- fixture setup time.

The missing/corrupt/truncated cases reuse the same 4096-archive fixture so the failure semantics are exercised
at the supported archive-count boundary rather than only on small unit fixtures.

## Interpretation

This is initially a **characterization gate**, not a latency SLA. It fails on correctness/fail-closed regressions
but does not yet reject a particular recovery duration.

After the first stable CI measurements are collected, v1 hardening must explicitly decide one of:

1. current worst-case recovery cost is acceptable and becomes the documented 4096-archive capacity contract; or
2. the measured cost is unacceptable and a follow-up optimization is required with a numeric target.

Do not introduce a persistent recovery index or segment-ID format solely from asymptotic complexity; use the
measured upper-bound behavior to justify any format/algorithm change.

## Non-claims

GitHub-hosted timing is useful for regression comparison but is not a target-server latency guarantee.
Physical storage characteristics, controller caches and power-loss behavior remain covered by the separate
Production v1 storage-validation gate.
