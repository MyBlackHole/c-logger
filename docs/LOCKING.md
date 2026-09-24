# Locking discipline

Locks protect invariants; they do not by themselves provide object lifetime.
Every lock must have a documented purpose, protected data set and ordering
relationship.

## Global lock hierarchy

The global logger lock order is:

```text
g_control_mu
    ->
g_lifetime_lock
    ->
logger instance locks
```

Code must not acquire `g_control_mu` while holding a global lifetime read pin
or an instance lock.

Responsibilities:

- `g_control_mu`: serializes lifecycle controllers such as init/shutdown.
- `g_lifetime_lock`: prevents destruction/publication changes while admitted
  global users hold a read-side pin.
- instance locks: protect state inside one `logger_t`.

The generation ticket is an admission/version mechanism, not a replacement for
the lifetime lock.

## Logger instance locks

### emit_mu

`emit_mu` serializes backend state and synchronous emission.

It protects operations that mutate or depend on:

- `file_backend` active descriptor/rotation state;
- `syslog_backend` connection and metrics state;
- backend sync/reopen/write ordering.

Do not wait for async worker progress while holding `emit_mu`.

### progress_mu

`progress_mu` protects the condition-variable protocol for async completion.

It protects:

- `completed_pos`;
- waits/broadcasts on `progress_cv`.

Atomic completion counters can be read independently according to their
documented memory order, but `completed_pos` is not an atomic replacement for
the condition-variable predicate.

### q.wait_mu

`q.wait_mu` is only the sleep/wakeup mutex for the worker's empty-queue
predicate and condition variable.

It does not serialize MPSC queue payload publication. Producers reserve and
publish slots through atomics; `q.wait_mu` only prevents a lost wakeup between
predicate testing and `pthread_cond_wait()`.

## Audit locking

Audit uses separate control and operation serialization. A resource-management
cleanup conversion must preserve the existing ordering and cancellation policy;
do not mechanically replace Audit lock/unlock pairs until the entire function's
lock graph is reviewed.

The persistent writer-lock fd is an ownership/exclusivity mechanism, not a
pthread lock and not a substitute for in-process serialization.

## Lock scope

Prefer the smallest lexical scope that preserves the invariant:

```c
{
    guard(mutex)(&lock);
    mutate_protected_state();
}
/* slow independent work here */
```

Do not move formatting, unrelated I/O, waits or callbacks under a lock simply
because a guard makes it syntactically convenient.

## Locking versus lifetime

These answer different questions:

- lock: "may two users mutate/read this invariant concurrently?"
- lifetime pin/ref/join: "can the object disappear while I use it?"

A pointer loaded under a lock is not automatically safe after unlocking unless
the lifetime protocol says so.

## Lock-order review

Before adding a lock acquisition, review:

1. what data/invariant it protects;
2. whether the caller already holds another lock;
3. the required global order;
4. whether any called function acquires another lock;
5. whether any callback can reenter the library;
6. whether the region can sleep or perform unbounded I/O;
7. whether cleanup/destructors run while the lock is still required.

Complex multi-lock functions should remain explicit until the full ordering can
be represented clearly with guards.
