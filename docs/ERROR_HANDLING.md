# Error handling

Error handling is part of the resource and durability contract. Cleanup must
not accidentally replace the error that explains the failed operation.

## Internal versus public return conventions

Internal helpers generally use:

```text
0        success
-negative errno
```

Public status APIs that follow the POSIX style use:

```text
0        success
-1       failure, errno = positive error
```

Do not mix the two conventions inside one function without an explicit
translation point.

## Preserve the first meaningful error

When work fails and cleanup also fails, decide which error is semantically
primary.

For ordinary rollback cleanup, preserve the work error:

```text
operation error
    |
best-effort cleanup
    |
return original error
```

This is why automatic free/close helpers preserve the incoming `errno`.

## Error-bearing finalization stays explicit

Some release operations are not "mere cleanup". Their failure is part of the
API/durability result, including cases such as:

- `fsync()`;
- directory sync after namespace changes;
- rotation/reopen transitions;
- final close when close error is intentionally observable;
- Audit checkpoint/state persistence.

Do not hide these behind a destructor that discards the result.

## Sticky backend errors

A logger instance records its first backend I/O error. Later success does not
erase evidence that earlier output failed.

Metrics distinguish:

- attempted/completed records;
- emitted records;
- failed records;
- first sticky error.

A flush may therefore complete current work yet still report the earlier sticky
error.

## Partial writes

A short/partial backend write is not silently promoted to success. Where
per-record acknowledgement cannot be reconstructed for a partial batch, the
implementation classifies the affected batch conservatively as unconfirmed.

## Validation errors

Reject invalid configuration before acquiring expensive or externally visible
resources whenever possible.

Do not silently mask:

- unknown output bits;
- invalid enums;
- invalid file modes;
- impossible queue capacities;
- unsupported platform/backend semantics.

## Cleanup and errno

Automatic cleanup for temporary fd/FILE ownership saves and restores `errno`.
Destructors must not accidentally turn:

```text
ENOMEM
```

into:

```text
EBADF from rollback close
```

unless the close itself is explicitly the operation being reported.

## Error-path tests

Constructor/error-path tests should cover failure after each meaningful
acquisition boundary and verify:

- no resource leak;
- no double release;
- no stale owner;
- no abandoned lock;
- correct object/thread count;
- correct returned error/errno;
- next-generation/retry behavior where supported.
