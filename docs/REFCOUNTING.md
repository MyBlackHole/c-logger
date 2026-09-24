# Reference-counting policy

c-logger does **not** currently use a generic per-object reference count for
`logger_t`, Audit runtime objects, queue storage, worker state, or global
logger lifetime.

Reference counting is one lifetime mechanism, not the default ownership model.

## When reference counting is appropriate

Use a reference count only when all of these are true:

1. more than one independent owner may keep the same object alive;
2. those owners can outlive the lexical scope or lock in which the pointer was
   obtained;
3. there is no simpler owner/join/pin protocol that defines the last user;
4. the object must remain alive until the last independent owner releases it.

If a single structural owner can stop/join all users before destroy, or if a
lifetime rwlock/pin already prevents destruction while borrowed, adding a
reference count is usually unnecessary.

## REFCOUNTED versus SHARED

- **SHARED**: multiple users exist, but lifetime is governed by a lock pin,
  join, generation gate, or externally enforced owner/borrow contract.
- **REFCOUNTED**: each independent lifetime owner holds a counted reference and
  the transition to zero runs final release.

A shared object is not automatically refcounted.

Current examples that remain non-refcounted:

- explicit `logger_t *`: host owns; SDK/callers borrow;
- global logger: generation/admission + lifetime rwlock pin;
- worker: logger owner + cooperative stop + join;
- queue/workspace: one structural owner + join-before-free;
- `live_objects`: process census/gate only, not object lifetime.

## Required semantics if introduced

### Initial reference

The creator receives exactly one initial owned reference. Publishing a pointer
does not silently manufacture another reference.

### Get requires a live object

A normal `get()` is only valid while another lifetime guarantee proves that
the object is still alive: an existing reference, a lock that excludes final
release, or a dedicated `try_get` that fails once the count reaches zero.

Loading a raw pointer atomically and incrementing later is not sufficient if a
final put can free the object in between.

### Put consumes ownership

`put()` consumes exactly one owned counted reference. The reference observing
the transition to zero owns final release. After put, that ownership is gone and
must not be reused.

### Zero is terminal

Do not resurrect an object after the count reaches zero. Final release may
destroy locks, close descriptors and free memory.

### Overflow and underflow are bugs

Do not implement lifetime references as an unchecked `_Atomic unsigned` with
raw increments/decrements. A future refcount abstraction must make overflow,
underflow, double-put and resurrection detectable or fail-safe, behind a narrow
API rather than exposing arbitrary counter arithmetic.

### Final release is explicit

A refcounted type must identify one final release path. Conceptually:

```c
object_t *object_get(object_t *obj);
bool object_try_get(object_t *obj);
void object_put(object_t *obj); /* zero -> object_release(obj) */
```

Callers must not invoke `object_release()` directly while counted references
exist.

## Interaction with lexical cleanup

A counted reference itself may be a lexical OWNED resource whose destructor
calls `put()`. Automatic cleanup then manages one reference, not the whole
shared object's lifetime protocol.

## Review checklist

Before introducing a refcount, answer:

1. Why is single ownership insufficient?
2. Why can join or a lifetime lock/pin not define the last user?
3. Where is the initial reference created?
4. Under what protection can a new reference be acquired?
5. Can lookup race the final put?
6. What operation consumes each reference?
7. What function runs on zero?
8. How are overflow, underflow, double-put and resurrection prevented?
9. Does this add atomic RMW to a hot path, and is the cost necessary?

Until a real object requires these semantics, do not add an unused generic
refcount implementation.
