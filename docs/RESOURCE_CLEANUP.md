# Linux-style internal resource management

c-logger uses an internal user-space implementation of Linux's scope-based
resource ownership model. The API shape and coding rules intentionally follow
Linux cleanup conventions, while implementation details that depend on kernel
ERR_PTR, lockdep, compiler annotations, or kernel-only resource types are not
copied.

This facility is private. It is not installed and does not change the public
Logger ABI.

## Supported primitives

The internal header `src/logger_cleanup.h` provides:

- `DEFINE_FREE(name, type, release)`
- `__free(name)`
- `no_free_ptr(ptr)`
- `return_ptr(ptr)`
- `retain_and_null_ptr(ptr)`
- `take_fd(fd)`
- `DEFINE_CLASS / EXTEND_CLASS / CLASS / CLASS_INIT / scoped_class`
- `DEFINE_GUARD / DEFINE_GUARD_COND`
- `guard / scoped_guard / scoped_cond_guard`
- `ACQUIRE / ACQUIRE_ERR`

Common user-space definitions are included for heap pointers, POSIX file
descriptors and `FILE *`.

## Mandatory ownership rules

### One resource, one owner

At every point a resource must have one clear owner. An `__free()` or
`CLASS()` variable owns its resource until ownership is explicitly transferred.

Do not copy an owning variable and then treat both copies as owners.

### Declare and initialize together

A cleanup-managed resource must normally be declared at the point it is
acquired:

```c
void *buffer __free(free) = malloc(size);
if (!buffer)
    return -ENOMEM;
```

Do not group cleanup variables at the top of a function and assign them much
later. Declaration order determines unwind order.

### LIFO is part of correctness

Cleanup runs in reverse declaration order. If B depends on A during teardown,
acquire/declare A before B.

This applies especially when guards and resource destructors interact. A
destructor that requires a lock must be declared after the guard so the
destructor runs before the guard unlocks.

### Do not mix goto-unwind with scope-unwind

For one function, either:

- keep the existing explicit `goto fail_*` resource-unwind model; or
- migrate the complete related unwind set to scope cleanup.

Do not partially convert a goto-based ownership graph. Goto can jump between
lexical scopes and makes cleanup ordering harder to review.

### Transfer ownership explicitly

Returning or publishing a cleanup-managed resource requires an explicit
ownership operation:

```c
struct item *item __free(free) = item_alloc();
if (!item)
    return NULL;

return_ptr(item);
```

Use `no_free_ptr()` when assigning the resource to a new owner, and
`retain_and_null_ptr()` only when another operation has consumed the resource
on success.

For POSIX descriptors, `take_fd()` moves ownership and sets the source to -1.

## Guards

`guard()` binds an unconditional lock/resource guard to the current lexical
scope. Prefer a nested block or `scoped_guard()` so the protected region is as
small as possible.

Conditional acquisition must expose failure. Use `ACQUIRE()` +
`ACQUIRE_ERR()`, or `scoped_cond_guard()`. c-logger's user-space conditional
guard normalizes positive pthread-style errno results to negative errno values.

Complex global lock ordering remains explicit until each function can be
converted as one ownership graph. Do not mechanically replace lock/unlock pairs.

## User-space differences from Linux

The model follows Linux, but a few representation details intentionally differ:

- ordinary POSIX fds use -1 as the invalid sentinel for `take_fd()`;
- conditional guards store error state explicitly instead of using ERR_PTR;
- kernel-only lockdep/context-analysis annotations are not reproduced;
- cleanup close/fclose preserves the incoming `errno`.

These differences preserve user-space API semantics while retaining the same
ownership discipline.

## What scope cleanup does not solve

Scope cleanup is not a general shared-lifetime mechanism. It does not make these
safe:

- asynchronous pthread cancellation;
- `longjmp`, `pthread_exit`, `_exit`, or signal-handler escape;
- cross-thread raw-pointer ownership;
- concurrent Logger destroy;
- worker lifetime and join ordering;
- MPSC slot ownership;
- global generation/lifetime pinning.

Those continue to use explicit state machines, cancellation cleanup, joins,
pins, locks, or reference ownership as appropriate.

## Error-bearing cleanup stays explicit

Automatic fd/FILE cleanup is for temporary ownership and rollback. Its
destructor intentionally ignores close/fclose failures while preserving the
previous `errno`.

If `close`, `fsync`, `closedir`, owner release, or another destructor
failure is part of an API result or durability decision, keep the operation
explicit. File-backend and Audit recovery finalization are examples.

## Initial production migrations

The first migrated production paths are intentionally simple:

- async worker workspace allocation rollback;
- MPSC queue slot allocation rollback.

They have single-threaded construction ownership and no error-bearing close
semantics. More complex file, Audit, and global-lock paths should be converted
only when their complete ownership graph is reviewed.
