# Resource ownership

Resource ownership is the first layer of c-logger's lifetime model. Automatic
cleanup, locking and atomics are implementation tools; none of them answers who
is responsible for releasing a resource.

For every non-trivial resource, review must answer:

1. Who creates or acquires it?
2. Who owns it now?
3. At exactly what operation does ownership transfer?
4. Who performs the final release?

If any answer is ambiguous, the design is incomplete.

## Ownership states

c-logger uses four ownership states.

### OWNED

The current variable, object or subsystem is responsible for final release.

Examples:

- a local pointer annotated with `__free(free)`;
- `logger_queue_t.slots` after queue initialization succeeds;
- `logger_t.worker_workspace` after construction publishes it;
- a live `logger_file_t` that owns its file/dir/coordination descriptors.

OWNED values may move, but must not be silently copied into a second owner.

### BORROWED

The code may use the resource for a bounded lifetime but must not release it.

Examples:

- an explicit `logger_t *` borrowed by an SDK from its host;
- `logger_t *` passed into a normal logging call;
- pointers into immutable configuration that are copied before the call returns.

BORROWED values do not receive `__free()`, CLASS destructors or another
implicit release action.

### MOVED

Ownership has transferred to another owner. The old owner is invalidated in the
same operation that performs the transfer.

Typical transitions:

```c
object->buffer = no_free_ptr(buffer);
return_ptr(object);
fd = take_fd(local_fd);
```

After a move, the source must be NULL, -1, or an equivalent empty state and must
not be dereferenced or released again.

### SHARED

Lifetime is controlled by an explicit shared-lifetime protocol rather than one
lexical owner.

Examples in c-logger include:

- the global logger protected by generation/admission plus the lifetime rwlock;
- the worker thread whose lifetime ends only after cooperative stop and join;
- state shared between queue producers and the single queue consumer.

SHARED does not mean "nobody owns it". There must still be a designated final
owner/releaser and a protocol that prevents release while users remain.

## Ownership transfer rules

Ownership transfer must be visible at the transfer site. A plain assignment of
an owning pointer to a long-lived object is not sufficient.

Preferred forms:

- local owner -> object field: `field = no_free_ptr(local)`;
- local owner -> caller: `return_ptr(local)`;
- callee consumes ownership on success: `retain_and_null_ptr(local)`;
- POSIX fd move: `take_fd(fd)`.

For movable structs such as `logger_file_t`, the source must be reset to its
documented empty value immediately after the move.

## Owner/releaser pairing

Every owning type or field should make its final releaser discoverable close to
its declaration or constructor/destructor pair.

Examples:

```text
logger_queue_t.slots
  acquire: calloc() in logger_queue_init()
  owner after publish: logger_queue_t
  final release: logger_queue_destroy()

logger_t.worker_workspace
  acquire: logger_worker_workspace_create()
  owner after publish: logger_t
  final release: logger_worker_workspace_destroy(), after worker join

global g_logger
  acquire: logger_create()
  owner after publication: global lifecycle controller
  users: lifetime-pinned BORROWED references only
  final release: logger_shutdown_status() / controlled teardown
```

## Public explicit logger ownership

The public explicit instance model remains host-owned:

```text
host creates logger_t
    |
    +-- SDK/callers borrow logger_t
    |
host stops and joins all borrowers
    |
host destroys logger_t
```

No internal cleanup helper turns a raw borrowed `logger_t *` into a safe
cross-thread reference. Concurrent destroy remains a caller contract violation.

## Ownership review checklist

For every resource-affecting change, verify:

| Question | Required evidence |
|---|---|
| creator/acquirer | exact function/expression |
| current owner | lexical variable, struct, subsystem or shared protocol |
| transfer point | exact statement/API and source invalidation |
| final releaser | exact destroy/close/join/ref-drop path |
| borrowed users | bounded lifetime and why release cannot race them |
| release failure | whether failure is semantically observable |
| teardown dependency | correct LIFO or explicit order |
| cross-thread use | explicit lifetime protocol |

See `RESOURCE_CLEANUP.md` for lexical cleanup after ownership is already
defined.
