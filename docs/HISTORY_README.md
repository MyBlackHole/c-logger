> 新增受控 `logger_fork_reinit()`：允许初始化之后 fork，父子无需 exec 即可重新初始化。
> 前提是先停止业务线程、关闭 Audit 与显式实例；helper 在父进程清理默认 Logger。
> 这不是任意多线程 raw fork 自动恢复。详见 `docs/CONTROLLED_FORK_REINIT.md`。

> **当前交付：Round 5 Fork/生产隔离修复，仍非 Production v1。**
> 子进程不能靠覆盖 pthread 对象重建运行时；继承状态必须 exec 后再初始化。
> 生产 `logger` 与 `logger_test_support` 分开构建，不由环境变量开启生产故障注入。
> 当前契约见 [ROUND5_FORK_ISOLATION](docs/ROUND5_FORK_ISOLATION.md)，实际验证及未通过项见 [ROUND5_RESULTS](validation/ROUND5_RESULTS.md)。
> Queue/Flush、Audit状态、密码与解析分别见 docs/ROUND1–4；剩余问题见 [KNOWN_ISSUES](docs/KNOWN_ISSUES.md)。
> 下方按迭代记录保留的测试/性能数字与设计叙述是历史，不能替代这些最新契约或发布门禁。

# Production C Logger

Linux/POSIX-oriented C11 logger.

Features:
- private process-global default logger plus explicit `logger_t` instances
- synchronous or bounded asynchronous logging
- TRACE/DEBUG/INFO/WARN/ERROR/FATAL levels
- stderr, rotating file and syslog outputs
- PID/TID/module/source metadata
- runtime level changes
- queue-overflow counter
- ERROR/FATAL synchronous fallback when the async queue is full
- graceful queue drain during shutdown
- parent pre-init stderr fallback; post-shutdown admission is closed

Build:

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

The default global facade deliberately keeps `g_logger` private in logger_global.c.
For audit/security logs, create a separate explicit logger instance rather than
mixing audit records into the operational logger.

## High-concurrency revision

The global facade now uses a process-wide `pthread_rwlock_t` only for logger
lifetime pinning. Normal producers take a shared/read lock, so producer threads
do not serialize on the old global mutex. Shutdown takes the write lock, detaches
the global pointer, then drains and destroys the logger safely.

The queue remains bounded. Queue-full behavior is intentionally non-blocking for
TRACE/DEBUG/INFO/WARN; ERROR/FATAL synchronously fall back to the backend.
A benchmark target (`bench_logger`) is included.

This is the conservative production optimization: it removes the global
serialization bottleneck while retaining straightforward shutdown correctness.
A true lock-free MPSC queue / epoch reclamation design is possible, but should
only be introduced after profiling demonstrates that the rwlock or queue mutex
is the next bottleneck.

## MPSC + batch revision

The async queue is now a bounded power-of-two MPSC sequence ring. Producers no
longer acquire the old queue mutex. The single consumer drains up to 64 records
per iteration and uses `writev()` for file/stderr batching.

The condition variable is only a sleep/wakeup mechanism for an empty queue; it
does not protect queue ownership. Shutdown sets `running=0`, wakes the consumer,
and the consumer drains all published records before exit.

Builds now use `-Werror` for the logger implementation.

## Backpressure and observability revision

Queue overflow policy is configurable per severity. Defaults are DROP for
TRACE/DEBUG/INFO/WARN and synchronous fallback for ERROR/FATAL. Metrics expose
per-level drops, queue high-watermark, successful enqueue count, synchronous
fallbacks, consumer batches, and consumer records.

These counters use relaxed atomics and are intended for telemetry rather than
transactional accounting. Queue depth/high-watermark is approximate under
concurrency by design.

## Timestamp rotation revision

Numeric `.1/.2/...` archives were removed. Active files keep stable names such
as `xxx.log` and `xxx.audit.log`. Archived files use UTC timestamps:

    xxx.20260922T144531.123456Z.log
    xxx.20260922T144531.123456Z.audit.log

Rotation modes are NONE, SIZE, DAILY, SIZE_DAILY and EXTERNAL. Retention is
expressed in days. Cleanup only deletes files matching the logger's exact
timestamp archive naming pattern. `logger_reopen[_instance]()` supports an
external logrotate-style owner without doing unsafe work in a signal handler.

## Audit subsystem

`audit.h` / `audit.c` adds a separate structured audit layer backed by an
independent logger instance. A service named `xxx` writes operational records
to `xxx.log` and audit records to `xxx.audit.log`.

Audit records use stable single-line key/value text, not JSON. String values are
quoted and escape backslash, quote, newline, carriage return, and tab. The audit
path is derived from `log_dir + name + ".audit.log"`.

The first implementation deliberately uses synchronous audit writes and can
fsync each record. This avoids silent queue drops. IMPORTANT: the current logger
backend write API returns void, so exact write/fsync failures cannot yet be
propagated through `audit_write()`. The next reliability step is to add a
status-returning backend path before using AUDIT_FAIL_DENY as a strict
security-operation gate.

## Strict audit I/O status revision

A status-returning synchronous backend API was added:

    logger_log_sync_status(...)

It bypasses the async queue, writes the record synchronously, forces `fsync()`,
and returns `0` or negative errno. `audit_write()` now uses this path and
converts backend failures to `-1` with `errno` set.

This makes local-file audit failures such as ENOSPC, EBADF, EIO, and fsync
errors observable by business logic, so a caller can implement fail-closed
security operations when configured with AUDIT_FAIL_DENY.

Note: libc `syslog()` does not provide end-to-end delivery acknowledgement.
Strict audit durability therefore relies on the dedicated local audit file, not
syslog as the sole backend.

## Audit transaction semantics

Security-sensitive operations can now use an ATTEMPT/RESULT pair sharing a
transaction id:

    audit_begin(&event);   // durable ATTEMPT before operation
    ... protected operation ...
    audit_end(&event, AUDIT_SUCCESS, 0);

With AUDIT_FAIL_DENY, callers should not execute the protected operation when
`audit_begin()` fails. The RESULT record documents the actual outcome. An
ATTEMPT without a RESULT after a crash is intentionally visible and can be
treated as an incomplete operation during audit review/recovery.

`seq` orders audit records inside the process lifetime; `txn` correlates the
ATTEMPT and RESULT records. They serve different purposes.

## CLI console frontend

`console.h` is intentionally separate from runtime logging and audit logging.

- `console_print()` is the stable stdout result/data channel and adds no labels
  or ANSI sequences.
- `console_info/warn/error()` are human diagnostics on stderr.
- `console_verbose/debug()` are controlled by verbosity.
- color supports AUTO/ALWAYS/NEVER; AUTO uses `isatty(stderr)`.
- console writes are serialized so messages from CLI worker threads do not
  interleave at stdio-call granularity.

Recommended mapping:
`--quiet` -> CONSOLE_QUIET, default -> NORMAL, `-v` -> VERBOSE,
`--debug` -> DEBUG, `--no-color` -> COLOR_NEVER.

This revision also fixes audit ATTEMPT semantics: ATTEMPT records no longer
claim `result=SUCCESS`; result/error fields are emitted only for RESULT records.

## Unified runtime record/source revision

Runtime log lines now standardize on:
`timestamp level pid tid [module] file:line function() message`.

Timestamp output uses local wall clock with six fractional digits and an
explicit numeric timezone offset, e.g.
`2026-09-22T22:35:41.123456+0800`.

`logger_source_t` / `LOGGER_SOURCE()` were added as the source-location model,
providing a stable place for module/file/function/line metadata without forcing
future public APIs to keep growing positional parameters. Existing LOG_* macros
remain source-compatible.

Console output intentionally remains free of runtime metadata. Audit output
keeps pid/tid but does not add source file/line/function because those are
implementation details rather than durable audit semantics.

## Detail policy and scoped request context

Runtime logger detail is now independent from severity:
MINIMAL = time/level/message; NORMAL adds module; VERBOSE adds pid/tid;
DEBUG adds request/session/trace context plus source file/line/function.

Request context is thread-local and copied into each record before async enqueue,
so worker formatting never references transient caller memory. Use
`logger_context_set()` at request entry and `logger_context_clear()` at exit.

Console DEBUG now has a source-aware `CONSOLE_DEBUG(...)` macro. Normal console
output remains clean. Audit schema remains fixed and is unaffected by logger
detail settings.

## Production-hardening review revision

This pass focuses on correctness rather than new surface features.

- batched `writev()` now handles EINTR and partial writes correctly;
- file mode is configurable; runtime logs default to 0640 and audit files to
  0600;
- timestamp rotation fsyncs the parent directory after rename/reopen to improve
  metadata durability across crashes;
- existing shutdown lifetime locking, bounded MPSC behavior, strict audit
  write/fsync status, timestamp archive matching, and TLS context copying were
  revalidated under the test suite.

Remaining design limits intentionally documented: fork-after-init is not
supported without an explicit atfork/reinit contract; libc syslog has
process-global state and no durable delivery acknowledgement; audit sequence
and transaction ids are still process-lifetime values rather than persistent
cross-restart identifiers.

## Audit process-instance identity

Each successful `audit_init()` generates a cryptographically strong 128-bit
instance id with Linux `getrandom()`, rendered as 32 lowercase hex characters.
Every audit record includes `instance=<id>`. `seq` and `txn` remain cheap
process-instance counters; their stable identities are therefore
`(instance,seq)` and `(instance,txn)`.

Audit initialization emits a durable `AUDIT_START` record. Normal shutdown emits
`AUDIT_STOP`. A subsequent START without a prior STOP for the previous instance
is useful evidence of an abnormal termination, though it does not by itself
prove the cause.

The instance id is intentionally not persisted or derived from PID/time.
Restarting creates a new identity.

## Audit integrity chain

Audit records now default to a SHA-256 hash chain. The hash covers the canonical
audit payload beginning at `instance=` through the `prev=` field; the ordinary
logger timestamp/PID/TID prefix is deliberately outside the integrity payload.

Each record carries `prev=<64 hex>` and `hash=<64 hex>`. The first record of an
audit instance uses an all-zero previous hash. `audit_verify_file(path)` detects
record modification and chain discontinuity inside one active/archived file.

This is tamper *detection*, not tamper prevention: an attacker able to rewrite
the complete file can recompute an unkeyed hash chain. External signed/WORM
checkpoints remain a later hardening layer.

Rotation continuity across separate files is not yet persisted/recovered in this
revision; that is intentionally the next step rather than pretending the
in-file verifier proves cross-file continuity.

## Audit chain restart / rotation continuity

The audit chain head is now persisted in a small 0600 state file:
`<name>.audit.state` (or `chain_state_path`). Each successful durable audit
record is followed by an atomic state update using temp-file + fsync + rename +
directory fsync. A later process instance loads that hash as its first `prev`.

This makes the chain continue across normal restart and timestamp rotation.
`audit_verify_file_from()` verifies an archived/current file from an expected
previous hash and returns its final hash, enabling ordered multi-file checking.

Crash caveat: audit record fsync and state-file replacement are two filesystem
transactions. A crash between them can leave a durable record newer than the
state anchor. Recovery/reconciliation of that narrow window is the next
hardening step; the implementation does not claim atomicity across two files.

## Audit checkpoint v1 and forward recovery

The state file is now a versioned checkpoint containing sequence, byte offset,
chain hash and CRC32. Startup seeks directly to the checkpoint offset and
validates complete records forward, advancing the checkpoint to the durable log
tail. A final non-newline partial record is treated as crash residue and
truncated to the last verified offset; corruption of a complete record fails
startup with EBADMSG.

The audit log remains the source of truth and the checkpoint is derived state.
Checkpoint updates remain atomic temp-file/fsync/rename/directory-fsync.

Current limitation: automatic reconciliation presently covers the current audit
file. Full discovery/order verification across timestamp archives after a crash
during rotation is still a separate next step.

## Rotation recovery pass

Startup recovery now discovers timestamp-named audit archives, sorts them by the
UTC timestamp embedded in the filename, and handles the important case where a
checkpoint offset refers to bytes that were moved out of the active file by a
rotation. It locates the checkpoint hash in archived history, verifies forward
from that point, then verifies the active file from its beginning.

A complete record with a broken `prev` or `hash` still fails closed. Only a
non-newline tail of the active file is eligible for truncation as crash residue.
Archived files are never silently truncated.

The archive discovery currently caps one recovery scan at 4096 matching files;
production deployments should keep retention bounded as already supported.

## Internal architecture refactor

The public API is unchanged, but audit implementation responsibilities are now
split:
- `audit.c`: lifecycle, event encoding, ATTEMPT/RESULT semantics and strict write
- `audit_integrity.c`: SHA-256/hash primitives
- `audit_recovery.c`: checkpoint persistence, archive discovery and crash recovery
- `audit_verify.c`: offline verification API
- `audit_internal.h`: private contracts only

This is intentionally an internal refactor: applications continue to include
only `audit.h`, `logger.h`, and `console.h`.

## Logger internal split — phase 1

Runtime formatting is now isolated in `logger_format.c` with private contracts in
`logger_internal.h`. The public `logger.h` API remains unchanged. This phase
deliberately does not move MPSC or file-backend state yet: concurrency-sensitive
code is being separated incrementally so each extraction is regression-tested.

## Logger internal split — phase 2

The bounded MPSC sequence ring has moved unchanged into `logger_queue.c` with a
private `logger_queue.h`. Queue initialization, enqueue, single-consumer pop,
batch drain and empty checks are now isolated from logger lifecycle/backend code.
The algorithm and memory-ordering choices were intentionally not changed during
this refactor.

## Logger internal split — phase 3
File open/write/writev, timestamp rotation, retention, reopen, offset and fsync behavior are now isolated in `logger_file.c` behind private `logger_file_t`. Public APIs remain unchanged.

## Logger internal split — phase 4

Consumer/backend emission moved to `logger_worker.c`. `logger_record.h` now owns
the private immutable record shape and breaks private-header dependency cycles.
`logger_internal.h` owns the private `struct logger` contract shared by core and
worker. `logger.c` is increasingly limited to lifecycle, record capture, public
APIs, context and the global facade.

## Logger internal split — phase 5

The process-global convenience facade (`logger_init`, `LOG_*`, global flush,
level, reopen and metrics) is now isolated in `logger_global.c`. The core
`logger.c` no longer owns the singleton lifetime lock/pointer. The private
vlogging entry point is explicitly declared in `logger_internal.h`; public API
and macro usage remain unchanged.

## Fork contract (current)

Use `logger_fork_reinit()` for a controlled single-threaded fork/continue after
initialization, then initialize each branch explicitly. The helper tears down
the optional default logger in the parent BEFORE fork; explicit instances and
Audit must already be closed by their owners. A remaining thread/instance is
rejected. See `docs/CONTROLLED_FORK_REINIT.md` and `examples/fork_reinit.c`.

Raw fork after initialization keeps the Round 5 ECHILD guard. There is no child
mutex/rwlock reset and no automatic restoration of arbitrary inherited state.
The old prepare/parent/child helpers are not this new controlled operation.

## Lifecycle state machine

`logger_t` now has an internal/publicly queryable lifecycle:
CREATED -> RUNNING -> STOPPING -> STOPPED. An explicit instance owner must
stop/join all users before destruction. The rejected historical active-call
counter approach is not used for pointer reclamation. Destruction drains/stops
the worker, then tears down backends.

The process-global facade still provides the stronger lifetime guarantee via its
rwlock: concurrent `LOG_*` producers can race with `logger_shutdown()` without
using a freed logger. Explicit `logger_t *` ownership still requires callers not
to invoke APIs after `logger_destroy()` returns; no C API can make a freed raw
pointer valid.

### Lifecycle ownership clarification

The attempted active-call pin counter was intentionally not used as reclamation:
a raw `logger_t *` caller can race between a zero-count observation and free, so
a counter alone cannot make arbitrary post-destroy pointer use safe. The global
facade uses its lifetime rwlock and is safe for shutdown/write races. Explicit
instances use the state machine to reject calls observed during STOPPING, but
the owner must still join/stop users before `logger_destroy()` returns.

### Shutdown admission gate

A shutdown admission flag is checked before global producers acquire the
lifetime read lock. `logger_shutdown()` closes admission before requesting the
write lock. This prevents continuous producer traffic from starving shutdown on
rwlock implementations that do not guarantee writer preference.

## Fault-injection verification framework

Private deterministic fault hooks now cover file write/fsync/rename/ftruncate
and audit checkpoint write/fsync/rename. Hooks are now compiled ONLY in logger_test_support. Production liblogger.a
ignores these variables even when they are set, and contains no crash-hook code.
The separate test archive uses LOGGER_FAULT_POINT/AFTER/ERRNO and short-write
hooks; only selected fault/crash tests link that archive.

The first fault matrix verifies strict synchronous logger propagation and audit
initialization/checkpoint failure propagation. Further crash-process tests can
now target exact durability boundaries instead of relying on nondeterministic
machine failures.

## Deterministic crash-point matrix

Fault verification now includes real child-process termination (`_exit(197)`)
at durability boundaries, followed by a fresh audit initialization and full
chain verification. Covered boundaries include: after durable audit record
fsync but before checkpoint update; before checkpoint rename; after checkpoint
rename; and after checkpoint commit before the in-memory chain head update.

These tests exercise restart reconciliation with actual process death rather
than merely returning EIO from a syscall.

## Sanitizer verification

CMake now supports `LOGGER_SANITIZE=address`, `undefined`,
`address-undefined`, or `thread`. A concurrency stress test exercises 12
producers plus concurrent level changes, metrics reads and flushes while the
async worker drains the MPSC queue. Explicit-instance ownership is respected:
all users are joined before destroy.

## Sanitizer runtime diagnosis

The execution environment was diagnosed with `readelf`/`ldd`. ASan binaries
were link-instrumented correctly, but the host test harness loaded another
library ahead of libasan. CMake now has `LOGGER_ASAN_PRELOAD` to make libasan
first for CTest without changing production linkage.

A standalone TSan probe is also used to distinguish a logger race from a broken
TSan runtime/platform. In this environment the probe result must be considered
before interpreting logger TSan failures.

## Public API contract review

A v1-candidate contract is now documented in `API.md`: ownership, threading,
fork, error model, audit durability and console channel semantics. The
implementation-only file-offset accessor was removed from the public header and
kept private. Audit now provides `audit_instance_id_copy()` so callers do not
need to retain a pointer to mutable process-global storage.

## Test profiles

CTest tests are now labeled by purpose (`unit`, `integration`, `concurrency`,
`fork`, `fault`, `crash`, `reliability`, `security`, `crypto`, `fast`).
`check-fast`, `check`, and `check-production` build targets plus
`scripts/check.sh` provide stable local/CI entry points. See `TESTING.md`.

## Benchmark matrix

`bench_matrix` and `scripts/benchmark.sh` provide a repeatable
1/2/4/8/16/32/64 producer × 64/256/1024/4000-byte matrix. Results include
producer/end-to-end throughput, drops, queue HWM, consumer records and batches
in CSV/JSONL. See `BENCHMARK.md`.
