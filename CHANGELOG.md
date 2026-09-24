# Changelog

## Unreleased — resource ownership audit

- Make the build dialect explicit as C11 plus GNU C extensions, matching the
  private Linux-style cleanup implementation.
- Add a repository ownership matrix covering heap objects, descriptors,
  FILE/DIR handles, workers, global lifetime pins and synchronization objects.
- Convert four single-owner rollback descriptors to `__free(close_fd)` with
  explicit `take_fd()` ownership transfer; keep error-bearing and shared
  finalization explicit.

## Unreleased — engineering invariants

- Add project-wide ownership, locking, concurrency, error-handling and lifecycle
  contracts around the Linux-style cleanup layer.
- Document logger field protection, worker/queue ownership, MPSC publication,
  global lock ordering and construct-before-publish/destruction rules.
- Make ownership and lifecycle design a review requirement rather than treating
  automatic cleanup as the primary resource model.


## Unreleased — internal resource ownership

- Adopt a Linux-style internal resource-management model: DEFINE_FREE/__free,
  no_free_ptr/return_ptr/retain_and_null_ptr, CLASS, guard/scoped_guard and
  ACQUIRE/ACQUIRE_ERR, independently implemented for user space.
- Define project rules for single ownership, declaration-at-acquisition, LIFO
  teardown, explicit ownership transfer, and no mixing of goto-unwind with
  scope-unwind in one resource graph.
- Require every managed resource to answer four ownership questions: creator,
  current owner, exact transfer point, and final releaser; distinguish OWNED,
  BORROWED, MOVED and SHARED lifetimes before choosing automatic cleanup.
- Convert async worker workspace construction and queue-slot initialization to
  automatic rollback while keeping concurrency/lifetime ownership explicit.
- Add regression coverage for free/return/retain/take ownership, class
  destructors, unconditional and conditional guards, errno preservation and
  acquisition-error reporting.


## Unreleased — resource footprint

- Move the async worker's batch records, formatted-line buffers and iovec
  scratch arrays from the worker stack into instance-owned heap workspace.
  Workspace capacity is capped at 256 records and shrinks with smaller queues.
- Extend benchmark JSON with queue slot/storage byte counts so the next queue
  payload redesign has an explicit memory baseline.


## Unreleased — production blocker hardening

- Prevent stderr EPIPE from delivering a new SIGPIPE to the host thread/process;
  preserve the caller's signal mask and process-global SIGPIPE disposition.
- Reject active file basenames in the internal `.logger.lock` / `.audit.lock`
  coordination namespaces so rotation cannot replace another owner's lock path.
- Reject unknown output bits and invalid Logger detail/rotation/file-mode/
  flush-level/overflow configuration values instead of silently accepting them.
- Add dedicated SIGPIPE, reserved-lock-namespace and invalid-config regressions.
- Remove release tests that required unsupported `ASYNC+ENABLE` entry into
  ordinary Logger/Console APIs. POSIX only guarantees three pthread cancellation
  control functions as async-cancel-safe; supported deferred and caller-disabled
  cancellation policies remain covered by contract tests.
- Add GitHub CI with a fortified shared Release profile, a complete
  unsanitized native Debug suite, and ASan/UBSan + TSan profiles. Private
  --wrap-based regression support disables FORTIFY only to keep libc hook symbol
  names stable; the production shared library remains fortified.


## 0.9.0 — release-engineering candidate

Not a final Production v1. Same complete Logger/Console/Audit functionality and
builtin SHA-256 only; no external crypto dependency or runtime implementation
changes in this revision.

- Linux ELF public symbol allowlist, hidden internals, LOGGER_0.9 symbol version
  and liblogger.so.0 SONAME; optional old fork helper remains opt-in.
- Relocatable install, CMake Logger::logger package, pkg-config, CPack TGZ and
  separate static/shared build profiles. Test support is not installed.
- Strict C++11-compatible default configuration/source macros, unchanged C
  definitions and data layout. Installed C and C++ consumers tested separately.
- A frozen prior-header consumer, layout/default checks, real dynamic symbol
  lookup and private-symbol rejection checks; PIC static embedding in SDK DSO.
- ELF/GLIBC baseline inspection gate; old target platform execution and durable
  media verification remain separate release requirements.

Compatibility: callers of undocumented internal symbols must migrate. Pre-1.0
package matching is exact; .so.0 is a candidate ABI, not a promise that all future
0.x snapshots are mutually compatible. Install static/shared variants into
separate clean prefixes. Do not exchange pointers across independent copies.
