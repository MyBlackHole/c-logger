#include "logger_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Only control operations take control_mu. Ordinary calls retain concurrent
 * read-side lifetime pins. Lock order: control -> lifetime -> instance locks.
 * No code obtains control while holding a lifetime read lock.
 */
static pthread_mutex_t g_control_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_lifetime_lock = PTHREAD_RWLOCK_INITIALIZER;
static logger_t *g_logger; /* lifetime lock; never returned to the application */

enum global_phase { G_IDLE, G_STARTING, G_RUNNING, G_STOPPING, G_STOPPED };
#define PHASE_BITS 3u
#define PHASE_MASK UINT64_C(7)
/* One atomic ticket contains both generation and phase; two independent loads
 * could otherwise associate a RUNNING state with the wrong generation. A new
 * init attempt consumes a generation even if candidate construction fails.
 */
static _Atomic uint64_t g_ticket;
static _Thread_local int g_in_global;
static _Thread_local int g_fork_prepared;

typedef struct {
	int old_cancel;
	int saved_errno;
	int control_locked;
	int lifetime_locked;
} global_scope_t;

static enum global_phase phase_of(uint64_t ticket)
{
	return (enum global_phase)(ticket & PHASE_MASK);
}

static uint64_t generation_of(uint64_t ticket)
{
	return ticket >> PHASE_BITS;
}

static uint64_t with_phase(uint64_t ticket, enum global_phase phase)
{
	return (ticket & ~PHASE_MASK) | (uint64_t)phase;
}

/* All resources/pins are released BEFORE cancellation can become effective.
 * An application's cancellation cleanup handler may subsequently use Logger.
 * This is deferral, not rollback or an I/O deadline. Explicit-instance APIs
 * additionally use their own inner scope; it preserves this disabled state.
 */
static int finish(global_scope_t *scope, int rc)
{
	int error;
	if (scope->lifetime_locked) {
		error = pthread_rwlock_unlock(&g_lifetime_lock);
		if (!rc && error)
			rc = -error;
	}
	if (scope->control_locked) {
		error = pthread_mutex_unlock(&g_control_mu);
		if (!rc && error)
			rc = -error;
	}
	error = rc < 0 ? -rc : scope->saved_errno;
	g_in_global = 0;
	errno = error;
	(void)pthread_setcancelstate(scope->old_cancel, NULL);
	errno = error;
	return rc < 0 ? -1 : 0;
}

static int begin(global_scope_t *scope)
{
	/* Check identity before touching inherited synchronization/once state. */
	if (logger_process_is_child()) {
		errno = ECHILD;
		return -1;
	}
	/* Reject direct same-thread reentry (e.g. from a printf/interposition
     * callback). This does not make signal handlers or cross-thread callback
     * cycles safe, and it does not allow pthread_exit/longjmp through a call.
     */
	if (g_in_global || logger_scope_busy()) {
		errno = EDEADLK;
		return -1;
	}
	*scope = (global_scope_t){ .saved_errno = errno };
	int rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					&scope->old_cancel);
	if (rc) {
		errno = rc;
		return -1;
	}
	g_in_global = 1;
	rc = logger_process_ensure();
	return rc ? finish(scope, rc) : 0;
}

static int lock_control(global_scope_t *scope)
{
	int rc = pthread_mutex_lock(&g_control_mu);
	if (!rc)
		scope->control_locked = 1;
	return -rc;
}

static int phase_error(uint64_t ticket)
{
	switch (phase_of(ticket)) {
	case G_IDLE:
		return ENODEV;
	case G_STARTING:
		return EAGAIN;
	case G_RUNNING:
		return 0;
	default:
		return ESHUTDOWN;
	}
}

static int pin(global_scope_t *scope, int allow_bootstrap)
{
	uint64_t ticket = atomic_load_explicit(&g_ticket, memory_order_acquire);
	int error = phase_error(ticket);
	if (error && !(allow_bootstrap && phase_of(ticket) == G_IDLE))
		return -error;
	/* Every read-side API uses this admission gate, not just LOG_* and flush.
     * After closure, continuous metrics/set/reopen calls do not join the rwlock
     * reader stream. Previously delayed callers can enter once but recheck.
     */
	int rc = pthread_rwlock_rdlock(&g_lifetime_lock);
	if (rc)
		return -rc;
	scope->lifetime_locked = 1;
	if (ticket != atomic_load_explicit(&g_ticket, memory_order_acquire))
		return -ESHUTDOWN;
	if (phase_of(ticket) == G_RUNNING && !g_logger)
		return -EIO; /* internal invariant, not a fallback-to-stderr condition */
	return 0;
}

int logger_init(const logger_config_t *cfg)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* No candidate files/sockets/worker on repeat init, including invalid cfg.
     * Fast reject also avoids a continuous repeat-init stream taking control.
     */
	if (phase_of(atomic_load_explicit(&g_ticket, memory_order_acquire)) ==
	    G_RUNNING)
		return finish(&scope, -EALREADY);
	int rc = lock_control(&scope);
	if (rc)
		return finish(&scope, rc);
	uint64_t old = atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (phase_of(old) == G_RUNNING)
		return finish(&scope, -EALREADY);
	if (generation_of(old) == (UINT64_MAX >> PHASE_BITS))
		return finish(&scope,
			      -EOVERFLOW); /* never reuse a generation */
	uint64_t ticket = ((generation_of(old) + 1u) << PHASE_BITS) |
			  G_STARTING;
	atomic_store_explicit(&g_ticket, ticket, memory_order_release);
	logger_t *candidate = logger_create(cfg);
	if (!candidate) {
		rc = -(errno ? errno : EIO);
		/* Failed bootstrap creation retains bootstrap logging, but consumes the
         * ticket so a pre-init delayed call cannot slip into a later instance.
         */
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		atomic_store_explicit(&g_ticket, with_phase(ticket, rollback),
				      memory_order_release);
		return finish(&scope, rc);
	}
	rc = pthread_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		(void)logger_destroy_status(candidate);
		enum global_phase rollback =
			phase_of(old) == G_IDLE ? G_IDLE : G_STOPPED;
		atomic_store_explicit(&g_ticket, with_phase(ticket, rollback),
				      memory_order_release);
		return finish(&scope, -rc);
	}
	scope.lifetime_locked = 1;
	g_logger = candidate;
	atomic_store_explicit(&g_ticket, with_phase(ticket, G_RUNNING),
			      memory_order_release);
	return finish(&scope, 0);
}

int logger_shutdown_status(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	/* A queued shutdown refers to the generation observed here, not whichever
     * instance happens to exist after waiting for another controller.
     */
	uint64_t observed =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	int rc = lock_control(&scope);
	if (rc)
		return finish(&scope, rc);
	uint64_t current =
		atomic_load_explicit(&g_ticket, memory_order_acquire);
	if (generation_of(observed) != generation_of(current))
		return finish(&scope, -ESHUTDOWN);
	if (phase_of(current) == G_STOPPED)
		return finish(
			&scope,
			0); /* completed stop is not a freed-object retry */
	if (phase_of(current) != G_RUNNING && phase_of(current) != G_IDLE)
		return finish(
			&scope,
			-EIO); /* controller owns STARTING/STOPPING fully */
	/* IDLE can still have a pinned bootstrap stderr writer. Close admission
     * and wait for that reader too, even though there is no logger object. */
	atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPING),
			      memory_order_release);
	rc = pthread_rwlock_wrlock(&g_lifetime_lock);
	if (rc) {
		/* No pointer/resource has been changed; admission may reopen. */
		atomic_store_explicit(&g_ticket, current, memory_order_release);
		return finish(&scope, -rc);
	}
	logger_t *old = g_logger;
	g_logger = NULL;
	pthread_rwlock_unlock(&g_lifetime_lock);
	/* control remains held through all I/O, worker join, close and owner unlock. */
	rc = logger_destroy_status(old) ? -(errno ? errno : EIO) : 0;
	atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPED),
			      memory_order_release);
	return finish(&scope, rc);
}

void logger_shutdown(void)
{
	(void)logger_shutdown_status();
}

int logger_flush_status(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = pin(&scope, 0);
	if (!rc && logger_flush_instance_status(g_logger))
		rc = -(errno ? errno : EIO);
	return finish(&scope, rc);
}

void logger_flush(void)
{
	(void)logger_flush_status();
}

void logger_set_level(logger_level_t level)
{
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = (unsigned)level <= LOGGER_OFF ? pin(&scope, 0) : -EINVAL;
	if (!rc)
		logger_set_instance_level(g_logger, level);
	(void)finish(&scope, rc);
}

uint64_t logger_global_dropped(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return 0;
	int rc = pin(&scope, 0);
	uint64_t dropped = rc ? 0 : logger_dropped(g_logger);
	(void)finish(&scope, rc);
	return dropped;
}

int logger_reopen(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = pin(&scope, 0);
	if (!rc && logger_reopen_instance(g_logger))
		rc = -(errno ? errno : EIO);
	return finish(&scope, rc);
}

void logger_get_global_metrics(logger_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = pin(&scope, 0);
	if (!rc)
		logger_get_metrics(g_logger, out);
	(void)finish(&scope, rc);
}

void logger_get_global_io_metrics(logger_io_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = pin(&scope, 0);
	if (!rc)
		logger_get_io_metrics(g_logger, out);
	(void)finish(&scope, rc);
}

void logger_global_write(logger_level_t level, const char *module,
			 const char *file, int line, const char *func,
			 const char *fmt, ...)
{
	global_scope_t scope;
	if (begin(&scope))
		return;
	int rc = !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL :
							 pin(&scope, 1);
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		if (g_logger) {
			logger_vlog_internal(g_logger, level, module, file,
					     line, func, fmt, ap);
		} else {
			/* Legacy application-only bootstrap output, before first successful
             * initialization/explicit shutdown. Its pin includes the I/O, so it
             * cannot print an old delayed message after a new publication.
             */
			char text[LOGGER_MESSAGE_MAX];
			int n = vsnprintf(text, sizeof(text), fmt, ap);
			if (n < 0)
				rc = -EILSEQ;
			else if (dprintf(STDERR_FILENO, "%-5s [%s] %s\n",
					 logger_level_name(level),
					 module ? module : "app", text) < 0)
				rc = -(errno ? errno : EIO);
		}
		va_end(ap);
	}
	(void)finish(&scope, rc);
}

int logger_prepare_fork(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -1;
	int rc = g_fork_prepared ? -EALREADY : 0;
	if (!rc)
		g_fork_prepared = 1;
	/* Marker only: no held lock, worker stop, queue flush or fork guarantee. */
	return finish(&scope, rc);
}

void logger_after_fork_parent(void)
{
	if (logger_process_is_child()) {
		errno = ECHILD;
		return;
	}
	g_fork_prepared = 0;
}

void logger_after_fork_child(void)
{
	logger_process_invalidate_child();
}

#if LOGGER_ENABLE_LEGACY_FORK_HELPER
int logger_global_stop_for_clean_fork(void)
{
	global_scope_t scope;
	if (begin(&scope))
		return -errno;
	int rc = pthread_mutex_trylock(&g_control_mu);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.control_locked = 1;
	rc = pthread_rwlock_trywrlock(&g_lifetime_lock);
	if (rc) {
		(void)finish(&scope, -rc);
		return -rc;
	}
	scope.lifetime_locked = 1;
	logger_t *l = g_logger;
	unsigned expected_objects = l ? 1u : 0u;
	unsigned expected_threads = 1u + (l && l->async_mode ? 1u : 0u);
	unsigned threads = 0;
	if (l && l->async_mode && pthread_equal(pthread_self(), l->worker))
		rc = -EDEADLK;
	else if (logger_process_object_count() != expected_objects)
		rc = -EBUSY;
	else {
		rc = logger_process_thread_count(&threads);
		if (!rc && threads != expected_threads)
			rc = -EBUSY;
	}
	if (!rc) {
		uint64_t current =
			atomic_load_explicit(&g_ticket, memory_order_acquire);
		atomic_store_explicit(&g_ticket,
				      with_phase(current, G_STOPPING),
				      memory_order_release);
		g_logger = NULL;
		pthread_rwlock_unlock(&g_lifetime_lock);
		scope.lifetime_locked = 0;
		rc = logger_dispose_internal(l);
		atomic_store_explicit(&g_ticket, with_phase(current, G_STOPPED),
				      memory_order_release);
	}
	(void)finish(&scope, rc);
	return rc;
}
#endif
