# Internal lexical resource cleanup

This is an internal implementation facility. It is not installed, exported, or
part of the public Logger ABI.

## Purpose

`src/logger_cleanup.h` provides small GCC/Clang
`__attribute__((cleanup))` helpers in the same general style as Linux scoped
cleanup helpers:

- `LOGGER_AUTO_FREE`: free an owned heap pointer at lexical scope exit.
- `LOGGER_AUTO_FD`: close an owned file descriptor at lexical scope exit.
- `LOGGER_AUTO_FILE`: fclose an owned `FILE *` at lexical scope exit.
- `LOGGER_TAKE_PTR(x)`: transfer pointer ownership out of an auto-cleanup
  variable and set the source to NULL.
- `LOGGER_TAKE_FD(x)`: transfer descriptor ownership and set the source to -1.

The first production migrations are intentionally limited to allocation
rollback in the async worker workspace and queue slot initialization.

## Ownership rules

An auto-cleanup variable has exactly one owner. When ownership is moved into a
long-lived object, the source must be disarmed with `LOGGER_TAKE_PTR` or
`LOGGER_TAKE_FD`.

Do not copy an owning auto-cleanup variable into another owning variable. A
plain copy creates two apparent owners and can double-release the same resource.

Cleanup runs in reverse declaration order. Code must not rely on a resource
whose cleanup has already run.

## What it does not solve

Lexical cleanup is not a general lifetime system. It does not make these cases
safe:

- `pthread_cancel` or asynchronous cancellation;
- `longjmp`, `pthread_exit`, `_exit`, or signal-handler escape;
- cross-thread ownership or concurrent destroy;
- Logger worker lifetime, global generation/pinning, or MPSC slot ownership.

Those continue to use their existing explicit lifecycle protocols.

## Error handling

Automatic fd/FILE cleanup is for rollback and temporary ownership. Its cleanup
preserves the caller's `errno` and deliberately ignores close/fclose errors.

If close/fsync/closedir errors are part of an API result or durability decision,
keep an explicit close path. In particular, file-backend and Audit recovery
finalization remain explicit.

## Compiler boundary

The mechanism requires GCC/Clang cleanup-attribute support. This is an internal
source-level requirement only; it does not change the C public ABI or installed
headers. Target-platform runtime validation remains governed by
`docs/PLATFORM_BASELINE.md`.
