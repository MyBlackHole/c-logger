# Resource ownership matrix

This matrix is the working audit of c-logger's important process, heap,
descriptor, thread and synchronization resources. It complements
`RESOURCE_OWNERSHIP.md`: the four required questions are recorded here against
the current implementation.

Status meanings:

- **AUTO**: lexical owner uses Linux-style automatic cleanup for rollback.
- **EXPLICIT**: finalization stays explicit because ordering or release errors
  are semantically meaningful.
- **SHARED**: lifetime is governed by a concurrency protocol, not lexical scope.
- **BORROWED**: this code may use the resource but never releases it.
- **REFCOUNTED**: independent lifetime owners hold counted references; currently
  no production resource uses this mode.

## Logger instance

| Resource | Acquire/create | Current/persistent owner | Transfer point | Final release | Mode |
|---|---|---|---|---|---|
| `logger_t` heap object | `calloc` in `create_logger` | constructor, then host/caller | successful constructor return | `logger_destroy_status/logger_dispose_internal` | EXPLICIT |
| process live-object token | `logger_process_object_acquire` | process-wide object census/gate | paired with logger construction/destruction | `logger_process_object_release` | SHARED (not REFCOUNTED) |
| `emit_mu` | `pthread_mutex_init` | `logger_t` | none | destroy after backend/worker teardown | EXPLICIT |
| `progress_mu/progress_cv` | pthread init | `logger_t` | none | destroy after worker join | EXPLICIT |
| queue storage `q.slots` | `calloc` in `logger_queue_init` | lexical owner, then `logger_queue_t` | `q->slots = no_free_ptr(slots)` | `logger_queue_destroy` | AUTO -> EXPLICIT |
| worker workspace | workspace constructor allocations | workspace object, then `logger_t` | `no_free_ptr/return_ptr` | after worker join via `logger_worker_workspace_destroy` | AUTO -> EXPLICIT |
| worker thread | `pthread_create` | `logger_t` | none | cooperative stop + `pthread_join` | SHARED/EXPLICIT |
| explicit API `logger_t *` arguments | host | host remains owner | none | host only | BORROWED |

Constructor cleanup remains an all-or-nothing explicit unwind graph. It is not
partially converted to `__free` while the function still uses goto-based
resource unwinding.

## Global logger

| Resource | Acquire/create | Owner/users | Transfer/publication | Final release | Mode |
|---|---|---|---|---|---|
| `g_logger` | `logger_create` candidate | global lifecycle controller | publish under control + lifetime protocol | shutdown controller | SHARED |
| global read-side use | existing `g_logger` | borrowed while lifetime read pin held | none | pin unlock, never destroy | BORROWED |
| `g_control_mu` | static init | process-global controller | none | process lifetime | SHARED |
| `g_lifetime_lock` | static init | global admission protocol | none | process lifetime | SHARED |
| generation/phase ticket | static atomic | global state machine | atomic generation transitions | process lifetime | SHARED |

The atomic ticket gates admission/versioning. It does not replace the lifetime
rwlock and does not itself keep `g_logger` alive. The lifetime read lock is a
pin: it prevents final release while borrowed, but it is not a counted
reference.

## File backend

| Resource | Acquire/create | Owner | Transfer point | Final release | Mode |
|---|---|---|---|---|---|
| bound directory fd `dir_fd` | `open` during file init/reserve | `logger_file_t` | successful backend construction/move | `logger_file_close_status` | EXPLICIT |
| coordination fd `lock_fd` | `openat` | `logger_file_t` | successful backend construction/move | `logger_file_close_status` | EXPLICIT |
| active fd `fd` | `open_candidate` | lexical candidate, then caller/backend | `take_fd(fd)`, then install | close/replace in backend teardown/rotation | AUTO -> EXPLICIT |
| retention scan fd | `openat(".", O_DIRECTORY)` | lexical variable | successful `fdopendir` consumes it | `closedir` | AUTO -> EXPLICIT |
| retention `DIR *` | `fdopendir` | retention function | none | explicit `closedir` because error matters | EXPLICIT |
| rotation replacement fd | `open_candidate` | rotate/reopen transaction | install only after validation/sync | old/new explicit close according to transaction result | EXPLICIT |

File close, fsync, directory sync and transactional replacement errors are part
of behavior; do not hide them behind best-effort destructors.

## Syslog backend

| Resource | Acquire/create | Owner | Transfer point | Final release | Mode |
|---|---|---|---|---|---|
| candidate socket fd | `socket` in `connect_once` | local connect transaction | `s->fd = fd` only after connect success | backend close/reconnect | EXPLICIT |
| installed socket fd | successful `connect_once` | `logger_syslog_t` | assignment after connect | `logger_syslog_close` / reconnect | EXPLICIT |

Candidate socket cleanup intentionally remains explicit because close failures
update syslog metrics. Replacing it with generic `__free(close_fd)` would lose
observable accounting.

## Audit subsystem

| Resource | Acquire/create | Owner | Transfer point | Final release | Mode |
|---|---|---|---|---|---|
| `audit_runtime_t` heap object | `calloc` during audit init | init transaction, then Audit global runtime | publication under operation/lifecycle protocol | `dispose_runtime` | EXPLICIT/SHARED |
| writer-lock fd | `open` in `acquire_writer_lock` | lexical fd, then runtime | `s->writer_lock_fd = take_fd(fd)` | `dispose_runtime` after logger teardown | AUTO -> EXPLICIT |
| audit log directory fd | open during init | `audit_runtime_t` | successful init | `dispose_runtime` | EXPLICIT |
| reserved log/checkpoint file owners | file reserve/init | `audit_runtime_t` / logger transaction | explicit movable `logger_file_t` handoff | corresponding file owner close | EXPLICIT |
| recovery regular-file fd | `open` in `open_regular` | lexical fd | successful `fdopen` consumes fd; source invalidated with `take_fd` | caller `fclose` | AUTO -> EXPLICIT |
| recovery `FILE *` | `fdopen/fopen` | recovery operation | function-specific | explicit `fclose`; errors may matter | EXPLICIT |
| recovery segment array | `calloc` | recovery operation | none | free each path then array | EXPLICIT |
| segment path strings | `strdup` | segment array element | stored in element | recovery cleanup loop | EXPLICIT |
| Audit control/operation mutexes | static init | Audit lifecycle | none | process lifetime | SHARED |

Audit durable finalization stays explicit because checkpoint/file close/sync
ordering and errors are part of recovery semantics.

## Process/fork guard

| Resource | Acquire/create | Owner | Transfer point | Final release | Mode |
|---|---|---|---|---|---|
| atfork registration | `pthread_once/pthread_atfork` | process | none | process lifetime | SHARED |
| live object count | atomic acquire/release | process lifecycle protocol | count increment/decrement | paired release | SHARED |
| `/proc/self/task DIR *` (legacy helper) | `opendir` | thread-count function | none | explicit `closedir` | EXPLICIT |

The legacy DIR close remains explicit because its failure participates in the
function result.

## Worker/queue shared lifetime

Queue slots and worker workspace have a single structural owner, but they are
used concurrently. Destruction order is therefore fixed:

```text
stop publication/running
    ->
wake worker
    ->
worker drains/exits
    ->
pthread_join
    ->
destroy workspace
    ->
destroy queue storage
```

Lexical cleanup cannot replace this join-before-free requirement.

## Reference-count audit

No current production resource requires REFCOUNTED lifetime:

- explicit logger instances have one host owner plus bounded borrowers;
- global logger readers hold an rwlock lifetime pin;
- worker lifetime terminates through cooperative stop and join;
- queue/workspace storage has one structural owner and join-before-free;
- Audit runtime is governed by its lifecycle/operation protocol;
- `live_objects` counts process objects for fork/lifecycle gating and does not
  control any individual object's final release.

If a future resource has independent owners that can escape these lifetime
boundaries, update this matrix and `REFCOUNTING.md` in the same change that
introduces its get/put protocol.

## Migration decisions from this audit

Migrated now:

- Audit writer-lock candidate fd;
- Audit recovery regular-file candidate fd before `fdopen`;
- file backend candidate fd returned by `open_candidate`;
- file retention directory fd before `fdopendir`.

Intentionally left explicit:

- logger constructor goto unwind;
- any close/fsync/closedir whose error is returned;
- syslog socket candidate cleanup with metrics side effects;
- global/Audit multi-lock lifetime protocols;
- worker thread and queue shared lifetime;
- recovery segment aggregate cleanup.

Future conversions should update this matrix in the same change that modifies
the ownership graph.
