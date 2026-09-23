> 缺陷收敛开发版，仍非 Production v1。当前 fork/生产构建规范见
> `docs/CONTROLLED_FORK_REINIT.md` 与 `docs/ROUND5_FORK_ISOLATION.md`。受控 fork 可重新初始化；raw fork 仍拒绝继承运行时。
> Queue/Flush、Audit 状态、密码与解析分别见 Round 1–4；剩余问题见 KNOWN_ISSUES。

# Public API contract (v1 candidate)

## Error model

Public constructors return a pointer or `NULL` with `errno`.
Public `int` control/audit/console operations use `0` success and `-1` with
`errno`, except `logger_log_sync_status()`, which is retained as an advanced
compatibility API and returns `0` or negative errno.

## Ownership

`logger_create()` transfers ownership of a logger instance to the caller.
`logger_destroy()` releases it. The owner must stop and join every thread that
uses an explicit `logger_t *` before destroy. A raw C pointer is not valid after
destroy.

The global `LOG_*` facade has library-managed lifetime synchronization and may
race with `logger_shutdown()`.

## Threading

Logging, level changes, metrics and flush operations are intended to be
concurrent on a live logger. File emission is serialized internally; async
records enter the bounded MPSC queue.

## Fork

There are now TWO intentionally different operations.

### Controlled fork, no exec: `pid_t logger_fork_reinit(void)`

This opt-in helper supports initialization followed by a controlled fork and
fresh initialization in both parent and child. The optional process-global
logger is drained, synchronized, destroyed and its worker joined BEFORE fork.
Both branches have no default logger and must call `logger_init()` explicitly.
No inherited queue, file descriptor, worker or logger object is reused.

Required preconditions: join application threads, shut down Audit, and destroy
all explicitly owned loggers first. The helper must be called by the remaining
application thread. Only the optional global worker may still exist on entry.
Other libraries must also be quiescent and support single-threaded fork/continue.
Signals/atfork handlers must NOT use this library or create threads during the
operation. It is not a fork wrapper for arbitrary live multithreaded programs.

Resource counts and `/proc/self/task` are checked before teardown. An extra
instance or thread returns EBUSY without touching the default logger. After
teardown, another task-count check requires exactly one Linux thread before the
actual fork. This is a fail-closed check, NOT a substitute for application
synchronization, nor a proof about third-party locks. /proc must be available.

Return is fork-like: >0 parent, 0 child, -1 with errno failure. Teardown/sync,
post-teardown validation or fork failure can leave the global logger stopped.
There is no automatic rollback or configuration restoration. Owners decide how
to restore service; do not assume a failed return leaves the old logger active.

The registered child guard still invalidates during atfork handlers. Only the
helper's private clean-fork token permits identity rebinding after fork returns.
The inherited pthread_once has already completed and locks are unlocked; no
pthread object is overwritten or reinitialized. The token is one-shot in each
process. The child request context is cleared; parent context and console
settings are retained. Do not call the legacy after-fork invalidation helpers
after `logger_fork_reinit()`; the new helper has already completed its protocol.
Explicit loggers can be recreated afterwards, and Audit creates a fresh session.
Parent/child Audit destinations should differ; the same-chain writer lock is
still enforced, not bypassed by identity rebinding.

### Raw `fork()` after runtime use

The Round 5 defensive behavior remains: child constructors/status operations
return ECHILD before inherited locks/once/allocations/I/O. Void runtime APIs
no-op and set ECHILD. `logger_log_sync_status()` keeps its negative-errno style.
The process identity remains sticky through ordinary shutdown for a raw fork.
Calling `logger_after_fork_child()` or `audit_after_fork_child()` does not enable
reinitialization. `logger_prepare_fork()`/`logger_after_fork_parent()` remain
legacy markers; they do not drain workers or implement the controlled helper.

Fork before any runtime use while truly single-threaded, or fork+exec, remain
supported alternatives. No support for vfork/raw clone/signal-handler logging,
or arbitrary multithreaded post-fork reconstruction. LOG arguments are evaluated
before the library's rejection checks. See `docs/CONTROLLED_FORK_REINIT.md`.

## Audit durability and failure state

Strict file output must succeed before committed sequence/hash advances. Checkpoint
failure pauses appends with CHECKPOINT_FAILED and keeps the confirmed head in memory;
`audit_flush()` repairs only this derived checkpoint, never rewrites the event.
Log write/fsync failure is conservatively IO_FAILED: no more append/STOP on that
runtime, even if the syscall failure goes away. Stop, inspect/reconcile, then reinit.

`audit_get_status()` is a diagnostic snapshot, not a concurrent transaction receipt.
`audit_shutdown_status()` releases runtime ownership and reports STOP/checkpoint or
saved I/O failure; the void shutdown API is a compatibility wrapper.
A failure return can follow a committed record. Do not automatically replay it.
Crypto and recovery still have known release blockers; see the current round document.

## Console

`console_print()` is the stable stdout data/result channel. Human diagnostics
use stderr. Runtime logger metadata must not be mixed into stdout results.

## Configuration ABI

`logger_config_t`, `audit_config_t`, and `console_config_t` carry `struct_size` and `version`. Default macros populate both. Unknown versions are rejected; undersized versioned structs are rejected. A temporary zero/zero compatibility path remains for manually zero-initialized source callers.

## Audit single-writer enforcement

`audit_init()` takes a non-blocking POSIX `fcntl(F_SETLK)` exclusive lock on
`<log_dir>/<name>.audit.lock`. A competing process receives `EBUSY`. The lock fd
is held by its owning runtime until output disposal ends. Init and shutdown share
one control protocol, so old cleanup cannot release a new runtime's lock.
Raw-fork children must exec before creating Audit. A successfully controlled
`logger_fork_reinit()` child can create fresh Audit after fork, because Audit
was shut down before entry. CLOEXEC still applies to fork+exec.
Advisory locks coordinate cooperating local processes, not arbitrary direct writers.
The application must not open/close or replace the active lock inode elsewhere.

The lock file is coordination metadata, not integrity evidence; its existence
alone does not mean the lock is held.

## Syslog backend

`LOGGER_OUT_SYSLOG` uses a per-`logger_t` connected Unix datagram socket rather
than libc `openlog()/syslog()/closelog()`. Instances therefore have independent
identities and lifecycle state. Linux defaults to `/dev/log`. Failure to connect
the configured syslog output causes logger creation to fail rather than silently
pretending that backend exists.

## Audit digest providers

Audit integrity is provider-based. SHA-256 and SM3 currently expose the same
32-byte digest contract to chain/checkpoint/recovery code. Checkpoint format v2
stores the algorithm identifier, preventing a chain from being reopened with a
different digest algorithm. `audit_verify_file_with()` verifies non-default
algorithms explicitly.

## Production crypto backend

Audit digest selection is separate from the crypto implementation backend.
`LOGGER_CRYPTO_BACKEND=builtin` keeps the self-contained SHA-256/SM3
implementation. `LOGGER_CRYPTO_BACKEND=openssl` routes SHA-256 and SM3 through
OpenSSL EVP and links `OpenSSL::Crypto`. `audit_crypto_backend()` exposes the
selected backend for diagnostics/compliance inventory.

The audit chain/checkpoint format records the digest algorithm, not the library
backend, so a SHA-256 or SM3 chain remains portable between compatible backend
implementations. Known-answer tests for SHA-256("abc") and SM3("abc") gate both
backends.

## Sensitive data

The library uses explicit redaction rather than heuristic secret detection.
`logger_redact()` returns the canonical redaction marker and
`logger_mask_secret()` creates a partially masked representation while refusing
to reveal the complete input. Audit fields are metadata-only; secret material
must be removed by the caller before constructing an event. See `SECURITY.md`.

## Reinitialization contract

The process-global logger and Audit are init-once per active lifetime.
A second or concurrent initialization returns `-1` with `errno=EALREADY`.
After shutdown in the same process, initialization is allowed again. Concurrent
initializers without shutdown have one successful publication. Audit serializes
its whole lifecycle; ordinary global Logger still creates candidates before its
publication lock, and simultaneous init/shutdown is a known limitation (see
KNOWN_ISSUES). Console initialization remains reconfigurable and its atomic
settings may be updated repeatedly.

## Explicit multi-instance logging

`logger_init()` owns only the optional process-global default logger and remains
init-once per active lifetime. Independent loggers are created with
`logger_create()` and destroyed with `logger_destroy()`.

For explicit instances, the public convenience macros preserve source location:

```c
LOGGER_INFO(network_logger, "network", "connected fd=%d", fd);
LOGGER_ERROR(storage_logger, "storage", "write failed errno=%d", err);
```

`LOGGER_LOG(instance, level, module, ...)` is the generic form. These macros do
not introduce global state; they forward to `logger_log()`. The owner must stop
and join all users before destroying an explicit instance.

## Audit lifecycle admission

START and its checkpoint succeed before publication. STOPPING closes admission before
waiting for the current operation; successful STOP is the last record. All seq/txn,
encoding and commit state are serialized. Failed initialization does not emit STOP.
Delayed calls crossing a shutdown/reinit generation return ESHUTDOWN. Concurrent init
and shutdown are serialized, not assumed safe based only on a pointer check.
See `docs/ROUND2_AUDIT_STATE.md` for status fields, cancellation and compatibility notes.

## Round 3: digest errors and CRYPTO_FAILED

Private digest callbacks now return 0 / -errno and leave output unchanged on
failure. All write, recovery and verifier callers check that result. An EVP
failure never substitutes a zero digest, invokes builtin as fallback, changes
the selected algorithm, or disables integrity. Public Audit APIs remain POSIX-
style 0 / -1 + errno; CRYPTO_FAILED distinguishes an engine problem from an
uncertain backend IO_FAILED result. Context allocation failures map to ENOMEM,
missing EVP methods to ENOTSUP, other EVP stage/length failures to EIO.

A runtime crypto failure is sticky until shutdown/reinitialization. No record,
committed_seq, chain head or checkpoint is advanced for the failing operation;
audit_flush cannot clear it and shutdown_status reports it without a fake STOP.
The new enum value is appended; existing numeric values and status layout are
unchanged. Read docs/ROUND3_CRYPTO.md for initialization side effects, transaction
ID reservation, verification output and historical bad-log limitations.

## Round 4: strict audit records and recovery

Verifier/recovery now use one bounded KV codec. Field order, quoted escaping,
integer ranges, exactly 64 lowercase hex digest characters and final LF are
checked. Trailing bytes/extra lines in checkpoint v2 are rejected. Malformed
records use EBADMSG, oversized input uses EOVERFLOW, underlying I/O/crypto
failures retain their error. No public function or struct layout was added.

Recovery validates all retained segments and locates the checkpoint using verified
record offset/sequence/hash. Unique digest edges, not filename time or file size,
determine segment order. Real EOF fragments eligible for repair are saved in a
unique 0600 `.tail.*` file and synchronized before active truncation. This is
not proof of a crash and is not an atomic cross-file rollback. Complete
hash-bearing records missing only LF are rejected rather than silently removed.

The historical logger prefix stays outside the digest domain. Noncanonical old
control bytes/fields, unavailable roots after retention, untrusted history and
v1 checkpoints are not automatically converted. See the Round 4 document for
limits, O(total retained bytes + N²) startup work, evidence handling and required
exclusive-writer/trusted-directory assumptions.

## Production/test separation

`logger` / `liblogger.a` is always the production target. It has no environment
fault/crash implementation or symbols, regardless of BUILD_TESTING. With
`BUILD_TESTING=ON`, the separate **logger_test_support** archive provides legacy
fault/crash hooks and the test syslog path override for explicitly selected tests.
Normal regressions, examples and benchmarks link the production target. Never link
both variants: CMake reports incompatible LOGGER_VARIANT interfaces. There is no
runtime option or CMake cache flag to turn production logger into a fault build.

Use `-DBUILD_TESTING=OFF` for a library-only build. `LOGGER_SYSLOG_PATH` is no
longer a production environment override; production uses /dev/log. A configurable
non-test socket path remains a future API consideration, not an implicit override.
