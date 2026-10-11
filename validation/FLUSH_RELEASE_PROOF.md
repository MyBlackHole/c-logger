# Flush after a failed synchronization release

## Scope

Baseline: `0682859f7375e4b8d1bf50c6e22d1f7df9fd5947` (main after #154).
This is a narrow production Logger failure-path fix. It changes no public ABI,
queue publication protocol, normal flush watermark, or backend durability policy.

An already-admitted flush can observe a worker's failed `emit_mu` unlock while
waiting for its completion watermark. Previously `logger_wait_for_output()`
returned the sticky lifecycle error, but `logger_flush_instance_status()` then
unconditionally acquired `emit_mu` to run its final synchronization. If the
failed unlock retained the mutex, the flush hung with cancellation disabled.
A Global flush also retained its lifetime reader pin, blocking shutdown.

After the wait, flush now checks the existing `synchronization_error` receipt
before entering the backend critical section. A known failed synchronization
release prevents another lock acquisition. The original wait error still wins
when present. Ordinary backend I/O errors and wait-acquisition errors without a
failed-release receipt still run the best-effort final file synchronization.
This does not promise recovery from arbitrary mutex corruption or make an
already-blocked lock operation interruptible.

## Deterministic regression

`tests/regression/test_flush_release_proof.c` uses link-time wrappers only;
production contains no new test hook. The fixture pauses an admitted flush at
its progress-mutex acquisition, lets a real worker append a record, and injects
one `EAGAIN` from its emit-mutex unlock without releasing that mutex. Once the
worker has published its failure and exited, the fixture resumes the flush.
It never replaces a forbidden reacquisition with a synthetic successful return:
the unfixed implementation really hangs on the retained mutex.

Scenarios:

- `explicit-proof`: return the sticky error without another emit lock or fsync.
- `explicit-cancel`: a pending cancellation completes after the resource scope
  has been released, rather than staying disabled forever in the lock wait.
- `global-proof`: a shutdown already waiting for the Global reader pin completes
  and retains the unproven instance; later initialization remains rejected.
- `global-cancel`: the same Global lifetime-pin assertion with pending cancellation.
- `io-error`: a prior backend fsync error remains sticky, but final flush still
  issues fsync.
- `wait-error`: a progress-mutex acquisition error without a release-proof failure
  likewise does not suppress the final fsync.

The fixture uses an atomic captured instance pointer because the worker may
enter its wrappers before `logger_create()` returns. A test-only join reaps that
exact worker before process exit. The failed instance remains rooted, retired,
and unretried. A separate lease acquisition still returns busy. Only fixture
filesystem names are removed after all assertions, with no successor owner
attempted afterward; the test does not reset the mutex or fake successful teardown.

Xmake registrations:

- `flush_release_proof_production_test/<scenario>`: six tests, `build_tests=y`
  and `build_shared=n`; links the actual production archive.
- `flush_release_proof_regression/<scenario>`: six tests,
  `build_regression_tests=y`; links the existing lockdep-enabled static
  regression archive, including when the selected production build is shared.

GNU `--wrap` does not intercept calls inside a production DSO. The shared
configuration's white-box results are not claimed as DSO-internal interception.

## Results

Focused tests were run with Xmake 3.1.1 on Linux x86_64:

| Configuration | Result |
|---|---|
| Release static, production + regression new cases | 12/12 passed |
| Existing related flush/global cancellation/lifecycle/constructor/destroy/lock cases | 78/78 passed |
| Debug ASan + UBSan, production + regression new cases | 12/12 passed |
| Debug TSan, production + regression new cases | 12/12 passed |
| Release shared configuration, static white-box new cases | 6/6 passed |
| Production Release archive, sanitizer archive, and shared artifact isolation checks | Passed; Audit-free, no fault hooks |

ASan was run with `ASAN_OPTIONS=detect_leaks=0` because this executor's leak
scanner is restricted; these results do not claim a successful LSan run.
UBSan and TSan used `halt_on_error=1`.

The focused command is:

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -c -m release -o /tmp/logger-flush-proof-build \
  --build_shared=n --build_tests=y --build_private_tests=n \
  --build_regression_tests=y
xmake build -j4 flush_release_proof_production_test flush_release_proof_regression
xmake test -j1 'flush_release_proof_*/*'
```

The related targets are `flush_pending_regression`, `flush_watermark_regression`,
`global_flush_regression`, `global_cancel_regression`, `global_lifecycle_regression`,
`constructor_rollback_regression`, `destroy_status_regression`, and
`lock_failure_regression`.

## Baseline counterexample

For a same-fixture comparison, compile main's `src/logger.c` from `0682859`
with production definitions and replace only `logger.c.o` in a copy of the
production static archive. Link the current fixture with:

```text
--wrap=logger_create
--wrap=pthread_mutex_lock
--wrap=pthread_mutex_unlock
--wrap=pthread_rwlock_wrlock
--wrap=fsync
```

All four proof/cancellation scenarios print
`unexpected reacquire of unproven emit_mu` and exit with GNU `timeout` status
124 under a two-second external limit. The ordinary `io-error` and `wait-error`
controls exit zero on both baseline and fixed code. This distinguishes the
repaired failed-release path from broad suppression of ordinary flush work.

No full-suite, remote-CI, or production-deployment pass is claimed by this
focused verification.
