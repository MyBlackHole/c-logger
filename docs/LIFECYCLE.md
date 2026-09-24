# Object and subsystem lifecycle

Lifecycle defines legal state transitions, publication points and final
destruction. Ownership and cleanup are subordinate to this model.

## Logger instance lifecycle

Conceptually:

```text
allocate
  ->
initialize mutex/cond/backends
  ->
initialize queue/workspace if async
  ->
start worker if async
  ->
RUNNING
  ->
STOPPING
  ->
stop + join worker
  ->
sync/close/release owned resources
  ->
STOPPED + free object
```

A `logger_t` must not become externally usable until construction is complete.

## Construct before publish

The general rule is:

```text
allocate privately
initialize completely
validate invariants
publish
```

Do not publish a pointer and then continue construction in a way that lets
another thread observe a partially initialized object.

## Ownership handoff

A successful constructor transfers ownership to the caller only at the return
boundary. Pending deferred cancellation is resolved before the raw pointer is
handed to the application.

After return, the host owns the explicit instance and is responsible for
stopping/joining all borrowers before destroy.

## Destruction consumes ownership

`logger_destroy_status()` consumes the owner's instance. Failure during final
I/O does not make the pointer retryable; the object is still destroyed according
to the documented contract.

Do not treat a STOPPED state observation as permission to reuse a freed pointer.

## Global lifecycle

Global phases are:

```text
IDLE
  -> STARTING
  -> RUNNING
  -> STOPPING
  -> STOPPED
```

A generation is consumed for a real initialization attempt. Delayed operations
are tied to the generation they observed and must not silently attach to a new
generation.

The default/global logger is never returned as an owned pointer to the caller;
global operations only borrow it while holding the lifetime admission/pin.

## Worker lifecycle

The worker is owned by its `logger_t`.

It is created only for async instances and stopped cooperatively. Resources that
the worker may access, including its workspace and queue storage, are released
only after `pthread_join()`.

## Backend lifecycle

File/syslog backends are fully initialized before the instance enters RUNNING.
Reopen/rotation may replace internal descriptors, but ownership transfer must be
transactional: the old usable state is not released before the replacement is
ready according to the backend contract.

## Audit lifecycle

Audit has its own subsystem state and single-writer ownership. Its state machine,
checkpoint/recovery and persistent writer lock are not folded into Logger
lexical cleanup. Audit transition errors and persistent-state writes remain
explicit.

## Illegal lifecycle shortcuts

Do not:

- free an object while borrowers may still execute;
- reuse memory merely because state says STOPPED;
- detach a worker without an explicit ownership handoff;
- publish half-initialized objects;
- reset pthread objects with memset as a substitute for destruction;
- use cleanup attributes as a substitute for join/refcount/pinning.
