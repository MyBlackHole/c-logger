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

## Ownership comes before cleanup

Automatic cleanup is an implementation mechanism, not the ownership model
itself. Before adding `__free()`, `CLASS()`, `guard()`, or any destructor,
the code review must answer four questions for every resource:

1. **Who creates/acquires it?**
2. **Who owns it right now?**
3. **At exactly what operation does ownership move?**
4. **Who performs the final release?**

If any answer is ambiguous, the resource is not ready to be converted to
automatic cleanup.

The expected lifetime is:

```text
creator/acquirer
      |
      v
lexical owner (__free / CLASS / guard)
      |
      +---- no transfer ----> scope destructor ----> released
      |
      +---- transfer -------> persistent/new owner
                                  |
                                  v
                           explicit final releaser
```

Cleanup helpers must make this ownership graph more visible, never hide it.

### Ownership states

Every non-trivial resource is treated as one of four states:

- **OWNED**: this variable/object is responsible for release.
- **BORROWED**: usable for a bounded lifetime, but must never release it.
- **MOVED**: ownership has been transferred; the old owner is invalidated
  (NULL, -1, or an equivalent empty state) and must not be dereferenced.
- **SHARED**: lifetime is governed by an explicit shared protocol such as a
  refcount, generation pin, worker join, or another synchronization contract.

Only OWNED lexical resources use `__free()`/CLASS destructors. BORROWED
references must not carry automatic cleanup. MOVED variables must be empty
after transfer. SHARED resources are not reduced to lexical cleanup.

### One resource, one owner

At every point a resource must have one clear owner. An `__free()` or
`CLASS()` variable owns its resource until ownership is explicitly transferred.

Do not copy an owning variable and then treat both copies as owners.

For long-lived struct fields, the owning type must document the final releaser
near the field or constructor/destructor pair. For ownership transfer, the
transfer operation should be visible at the assignment site, for example:

```c
object->buffer = no_free_ptr(buffer);  /* local owner -> object */
```

After that statement `buffer` is MOVED and `object` is the new owner.

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

Ownership transfer is a state transition, not just pointer assignment. The old
owner must be invalidated in the same expression that publishes the resource to
the new owner.

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

Use these operations according to intent:

- `no_free_ptr(x)`: local lexical owner -> another owner.
- `return_ptr(x)`: local lexical owner -> caller.
- `retain_and_null_ptr(x)`: a called operation consumed ownership on success.
- `take_fd(fd)`: fd owner -> another owner/caller.

Plain assignment of an OWNED resource to a new long-lived owner without one of
these explicit transfer operations is a review failure.

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

## Review checklist

Every resource-affecting change must be reviewable with this checklist:

| Question | Required answer |
|---|---|
| Who creates/acquires it? | exact function/expression |
| Who owns it now? | lexical variable, struct field, subsystem, or shared protocol |
| When does ownership move? | exact statement/API and source invalidation |
| Who finally releases it? | exact destructor/destroy/close/join/ref-drop path |
| Is it borrowed instead? | if yes, no cleanup annotation is allowed |
| Can release fail meaningfully? | if yes, keep the error-bearing finalization explicit |
| Does teardown depend on another resource? | declaration order must encode correct LIFO |
| Is the lifetime cross-thread/shared? | use the explicit shared protocol, not lexical cleanup |

## Initial production migrations

The first migrated production paths are intentionally simple:

- async worker workspace allocation rollback:
  creator = `calloc/malloc`; current owner = local `__free` variable;
  transfer = `no_free_ptr()/return_ptr()`; persistent owner =
  `logger_worker_workspace_t` / `logger_t`; final releaser =
  `logger_worker_workspace_destroy()`.
- MPSC queue slot allocation rollback:
  creator = `calloc`; current owner = local `slots __free(free)`;
  transfer = `q->slots = no_free_ptr(slots)`; persistent owner =
  `logger_queue_t`; final releaser = `logger_queue_destroy()`.

They have single-threaded construction ownership and no error-bearing close
semantics. More complex file, Audit, and global-lock paths should be converted
only when their complete ownership graph is reviewed.
