#define _GNU_SOURCE
#include "logger.h"
#include "logger_internal.h"
#include "logger_queue.h"
#include "logger_file.h"
#include "logger_cleanup.h"
#include "logger_uapi.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#define PATH_MAX_ 4096
typedef logger_message_t msg_t;
static int reject_access(void)
{
	if (logger_process_is_child()) {
		errno = ECHILD;
		return 1;
	}
	if (logger_scope_busy()) {
		errno = EDEADLK;
		return 1;
	}
	return 0;
}

logger_detail_t logger_internal_detail(const logger_t *l)
{
	return l->detail;
}
int logger_internal_include_pid(const logger_t *l)
{
	return l->include_pid;
}
int logger_internal_include_tid(const logger_t *l)
{
	return l->include_tid;
}
int logger_internal_include_source(const logger_t *l)
{
	return l->include_source;
}
static _Thread_local logger_context_t g_ctx;
static _Thread_local char g_request_id[64], g_session_id[64], g_trace_id[64];
static void copy_ctx(char *d, size_t n, const char *s)
{
	if (!s) {
		d[0] = 0;
		return;
	}
	snprintf(d, n, "%s", s);
}
void logger_context_set(const logger_context_t *c)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	/* Stage all fields, allowing set(get()) and overlapping/swapped TLS input. */
	char request[64] = { 0 }, session[64] = { 0 }, trace[64] = { 0 };
	copy_ctx(request, sizeof(request), c ? c->request_id : NULL);
	copy_ctx(session, sizeof(session), c ? c->session_id : NULL);
	copy_ctx(trace, sizeof(trace), c ? c->trace_id : NULL);
	memcpy(g_request_id, request, sizeof(request));
	memcpy(g_session_id, session, sizeof(session));
	memcpy(g_trace_id, trace, sizeof(trace));
	g_ctx.request_id = g_request_id[0] ? g_request_id : NULL;
	g_ctx.session_id = g_session_id[0] ? g_session_id : NULL;
	g_ctx.trace_id = g_trace_id[0] ? g_trace_id : NULL;
	(void)logger_scope_end(&scope, 0);
}
void logger_context_clear(void)
{
	logger_context_set(NULL);
}
logger_context_t logger_context_get(void)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return (logger_context_t){ 0 };
	logger_context_t out = g_ctx;
	(void)logger_scope_end(&scope, 0);
	return out;
}
static void capture_context(msg_t *m)
{
	logger_context_t c = g_ctx;
	copy_ctx(m->request_id, sizeof(m->request_id), c.request_id);
	copy_ctx(m->session_id, sizeof(m->session_id), c.session_id);
	copy_ctx(m->trace_id, sizeof(m->trace_id), c.trace_id);
}

static pid_t tid_(void)
{
	return (pid_t)syscall(SYS_gettid);
}

static unsigned capture_mask_from_config(const logger_config_t *cfg)
{
	unsigned mask = 0;
	if (cfg->detail >= LOGGER_DETAIL_NORMAL)
		mask |= LOGGER_CAPTURE_MODULE;
	if (cfg->detail >= LOGGER_DETAIL_VERBOSE) {
		if (cfg->include_pid)
			mask |= LOGGER_CAPTURE_PID;
		if (cfg->include_tid)
			mask |= LOGGER_CAPTURE_TID;
	}
	if (cfg->detail >= LOGGER_DETAIL_DEBUG) {
		mask |= LOGGER_CAPTURE_CONTEXT;
		if (cfg->include_source)
			mask |= LOGGER_CAPTURE_SOURCE;
	}
	return mask;
}

static void prepare_record_metadata(logger_t *l, msg_t *m, const char *module,
				    const char *file, int line,
				    const char *function)
{
	unsigned mask = l->capture_mask;
	m->metadata_mask = (uint8_t)(mask & LOGGER_CAPTURE_RECORD_MASK);
	if (mask & LOGGER_CAPTURE_MODULE)
		m->module = module;
	if (mask & LOGGER_CAPTURE_PID)
		m->pid = l->pid;
	if (mask & LOGGER_CAPTURE_TID)
		m->tid = tid_();
	if (mask & LOGGER_CAPTURE_SOURCE) {
		m->file = file;
		m->line = line;
		m->func = function;
	}
	if (mask & LOGGER_CAPTURE_CONTEXT)
		capture_context(m);
}

logger_state_t logger_get_state(const logger_t *l)
{
	if (reject_access())
		return LOGGER_STATE_STOPPED;
	return l ? atomic_load_explicit(&l->state, memory_order_acquire) :
		   LOGGER_STATE_STOPPED;
}
static void update_high_watermark(logger_t *l)
{
	uint64_t depth = (uint64_t)logger_queue_depth(&l->q);
	uint64_t old = atomic_load_explicit(&l->queue_high_watermark,
					    memory_order_relaxed);
	while (depth > old &&
	       !atomic_compare_exchange_weak_explicit(
		       &l->queue_high_watermark, &old, depth,
		       memory_order_relaxed, memory_order_relaxed)) {
	}
}

static void vlog_body(logger_t *l, logger_level_t lv, const char *mod,
		      const char *f, int line, const char *fn, const char *fmt,
		      va_list ap)
{
	if (!l || !fmt || (unsigned)lv >= LOGGER_OFF ||
	    lv < atomic_load_explicit(&l->level, memory_order_relaxed))
		return;
	msg_t m = { .level = lv };
	prepare_record_metadata(l, &m, mod, f, line, fn);
	if (clock_gettime(CLOCK_REALTIME, &m.ts) != 0)
		return;
	int formatted = vsnprintf(m.text, sizeof(m.text), fmt, ap);
	if (formatted < 0)
		return;
	size_t stored = (size_t)formatted < sizeof(m.text) ?
				(size_t)formatted :
				sizeof(m.text) - 1u;
	m.text_len = (uint16_t)stored;

	if (!l->async_mode) {
		(void)logger_emit_status(l, &m, 0);
	} else if (logger_queue_push(&l->q, &m)) {
		atomic_fetch_add_explicit(&l->enqueued, 1,
					  memory_order_relaxed);
		update_high_watermark(l);
	} else if (l->overflow[lv] == LOGGER_OVERFLOW_SYNC) {
		/* sync fallback 不是 queue drop；实际 backend 结果由独立 I/O metrics 记录。 */
		atomic_fetch_add_explicit(&l->sync_fallbacks, 1,
					  memory_order_relaxed);
		(void)logger_emit_status(l, &m, 0);
	} else {
		atomic_fetch_add_explicit(&l->dropped_by_level[lv], 1,
					  memory_order_relaxed);
	}
}
void logger_vlog_internal(logger_t *l, logger_level_t level, const char *module,
			  const char *file, int line, const char *function,
			  const char *fmt, va_list args)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	if (l && atomic_load_explicit(&l->state, memory_order_acquire) ==
			 LOGGER_STATE_RUNNING)
		vlog_body(l, level, module, file, line, function, fmt, args);
	(void)logger_scope_end(&scope, 0);
}

static int copy_logger_config(const logger_config_t *in,
			      logger_config_t *out)
{
	if (!in || !out) {
		errno = EINVAL;
		return -1;
	}
	_Static_assert(LOGGER_CONFIG_SIZE == sizeof(*out),
		       "Logger v1 config layout changed");

	uint32_t size, version;
	memcpy(&size, (const unsigned char *)in, sizeof(size));
	memcpy(&version, (const unsigned char *)in + sizeof(size),
	       sizeof(version));
	int rc = logger_uapi_copy_struct(out, sizeof(*out), in, size, version,
					LOGGER_CONFIG_VERSION);
	if (rc) {
		errno = -rc;
		return -1;
	}
	out->struct_size = sizeof(*out);
	out->version = LOGGER_CONFIG_VERSION;
	return 0;
}
static int validate_logger_config(const logger_config_t *cfg)
{
	const unsigned output_mask =
		LOGGER_OUT_STDERR | LOGGER_OUT_FILE | LOGGER_OUT_SYSLOG;
	if ((unsigned)cfg->level > LOGGER_OFF ||
	    (unsigned)cfg->detail > LOGGER_DETAIL_DEBUG || !cfg->outputs ||
	    (cfg->outputs & ~output_mask) ||
	    (unsigned)cfg->rotation.mode > LOGGER_ROTATE_EXTERNAL ||
	    (cfg->file_mode & ~0777u) ||
	    (unsigned)cfg->flush_level > LOGGER_OFF)
		return -EINVAL;
	for (unsigned i = 0; i < 6; ++i)
		if ((unsigned)cfg->overflow[i] > LOGGER_OVERFLOW_SYNC)
			return -EINVAL;
	return 0;
}

typedef struct {
	int emit_mu_ready;
	int progress_mu_ready;
	int progress_cv_ready;
	int queue_ready;
	int worker_started;
} logger_ctor_state_t;

static void logger_ctor_note_proof_error(logger_t *l, int rc)
{
	int error = rc < 0 ? -rc : rc;
	if (!l || !error)
		return;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
		&l->lifecycle_error, &expected, error,
		memory_order_release, memory_order_relaxed);
}

/*
 * Roll back only resources whose construction completed. A synchronization,
 * join or census failure is a lifetime-proof failure: stop immediately and
 * intentionally retain the unpublished container. The caller keeps the
 * original construction errno; this return value is cleanup diagnostics.
 */
static int logger_ctor_rollback(logger_t *l, const logger_ctor_state_t *state)
{
	if (!l)
		return logger_process_object_release();

	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPING,
			      memory_order_release);

	if (state->worker_started) {
		atomic_store_explicit(&l->running, 0, memory_order_release);
		if (state->queue_ready)
			logger_queue_wake_force(&l->q);
		int join_rc = pthread_join(l->worker, NULL);
		if (join_rc) {
			logger_ctor_note_proof_error(l, join_rc);
			return -join_rc;
		}
	}

	logger_worker_workspace_destroy(l->worker_workspace);
	l->worker_workspace = NULL;

	if (state->queue_ready) {
		int rc = logger_queue_destroy(&l->q);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return rc;
		}
	}

	if (state->progress_cv_ready) {
		int rc = pthread_cond_destroy(&l->progress_cv);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}
	if (state->progress_mu_ready) {
		int rc = pthread_mutex_destroy(&l->progress_mu);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}
	if (state->emit_mu_ready) {
		int rc = pthread_mutex_destroy(&l->emit_mu);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}

	/* close errors are secondary to the construction error and do not make
	 * already-proven memory lifetime unsafe. Linux close is never retried. */
	int cleanup_rc = logger_file_close_status(&l->file_backend);
	logger_syslog_close(&l->syslog_backend);

	int release_rc = logger_process_object_release();
	if (release_rc) {
		logger_ctor_note_proof_error(l, release_rc);
		return release_rc;
	}

	free(l);
	return cleanup_rc;
}

static logger_t *create_logger(const logger_config_t *input,
			       logger_file_t *reserved)
{
	logger_config_t cfg;
	logger_ctor_state_t state = { 0 };
	logger_state_t expected = LOGGER_STATE_CREATED;
	int rc;
	if (copy_logger_config(input, &cfg) != 0)
		return NULL;
	rc = validate_logger_config(&cfg);
	if (rc) {
		errno = -rc;
		return NULL;
	}
	rc = logger_process_object_acquire();
	if (rc) {
		errno = -rc;
		return NULL;
	}
	logger_t *l = calloc(1, sizeof(*l));
	if (!l) {
		int primary = errno ? errno : ENOMEM;
		/* No container exists to retain. A failed census rollback still leaves
		 * the process guard conservative; never replace the primary ENOMEM. */
		(void)logger_ctor_rollback(NULL, &state);
		errno = primary;
		return NULL;
	}
	l->file_backend = LOGGER_FILE_EMPTY;
	l->syslog_backend.fd = -1;
	atomic_init(&l->level, cfg.level);
	atomic_init(&l->state, LOGGER_STATE_CREATED);
	atomic_init(&l->running, 0);
	atomic_init(&l->enqueued, 0);
	atomic_init(&l->sync_fallbacks, 0);
	atomic_init(&l->queue_high_watermark, 0);
	atomic_init(&l->consumer_batches, 0);
	atomic_init(&l->consumer_records, 0);
	atomic_init(&l->async_completed, 0);
	atomic_init(&l->sync_completed, 0);
	atomic_init(&l->emitted_records, 0);
	atomic_init(&l->failed_records, 0);
	atomic_init(&l->first_error, 0);
	atomic_init(&l->lifecycle_error, 0);
	for (unsigned i = 0; i < 6; ++i) {
		atomic_init(&l->dropped_by_level[i], 0);
		l->overflow[i] = cfg.overflow[i];
	}
	l->detail = cfg.detail;
	l->outputs = cfg.outputs;
	l->file_mode = (mode_t)(cfg.file_mode ? cfg.file_mode : 0640);
	l->async_mode = !!cfg.async_mode;
	l->flush_level = cfg.flush_level;
	l->include_pid = cfg.include_pid;
	l->include_tid = cfg.include_tid;
	l->include_source = cfg.include_source;
	l->capture_mask = capture_mask_from_config(&cfg);
	l->pid = (l->capture_mask & LOGGER_CAPTURE_PID) ? getpid() : 0;
	snprintf(l->ident, sizeof(l->ident), "%s",
		 cfg.ident ? cfg.ident : "app");

	rc = pthread_mutex_init(&l->emit_mu, NULL);
	if (rc != 0)
		goto fail;
	state.emit_mu_ready = 1;

	rc = pthread_mutex_init(&l->progress_mu, NULL);
	if (rc != 0)
		goto fail;
	state.progress_mu_ready = 1;

	rc = pthread_cond_init(&l->progress_cv, NULL);
	if (rc != 0)
		goto fail;
	state.progress_cv_ready = 1;

	if (reserved) {
		if (!(l->outputs & LOGGER_OUT_FILE) ||
		    logger_file_open_reserved(reserved)) {
			rc = errno ? errno : EINVAL;
			goto fail;
		}
		l->file_backend = *reserved;
		*reserved =
			LOGGER_FILE_EMPTY; /* move, never unlock a shared lease */
	} else if ((l->outputs & LOGGER_OUT_FILE) &&
		   logger_file_init(&l->file_backend, cfg.file_path,
				    cfg.rotation, l->file_mode)) {
		rc = errno ? errno : EIO;
		goto fail;
	}

	if ((l->outputs & LOGGER_OUT_SYSLOG) &&
	    logger_syslog_init(&l->syslog_backend, cfg.ident, &cfg.syslog) !=
		    0) {
		rc = errno ? errno : EIO;
		goto fail;
	}

	/* Deferred startup is explicit, not a claim that the sink is healthy. */
	if (l->outputs & LOGGER_OUT_SYSLOG)
		logger_note_io_error(l, l->syslog_backend.metrics.last_error);

	if (l->async_mode) {
		if (logger_queue_init(&l->q, cfg.queue_capacity ?
						     cfg.queue_capacity :
						     8192) != 0) {
			rc = errno ? errno : EIO;
			goto fail;
		}
		state.queue_ready = 1;
		l->worker_workspace = logger_worker_workspace_create(l->q.cap);
		if (!l->worker_workspace) {
			rc = errno ? errno : ENOMEM;
			goto fail;
		}
		atomic_store_explicit(&l->running, 1, memory_order_release);
		rc = pthread_create(&l->worker, NULL, logger_worker_main, l);
		if (rc != 0) {
			atomic_store_explicit(&l->running, 0,
					      memory_order_release);
			goto fail;
		}
		state.worker_started = 1;
	}

	/*
	 * The worker can fail before pthread_create() returns. Only CREATED may
	 * become RUNNING: never overwrite its release-published STOPPING state.
	 * A failed acquire observes the preceding lifecycle_error publication;
	 * rollback still stops and joins every successfully created worker.
	 */
	if (!atomic_compare_exchange_strong_explicit(
		    &l->state, &expected, LOGGER_STATE_RUNNING,
		    memory_order_acq_rel, memory_order_acquire)) {
		rc = atomic_load_explicit(&l->lifecycle_error,
					  memory_order_acquire);
		if (!rc)
			rc = EIO;
		goto fail;
	}
	return l;

fail: {
	int primary = rc ? rc : EIO;
	/*
	 * Cleanup diagnostics never replace the primary construction failure.
	 * logger_ctor_rollback() itself enforces proof-before-free.
	 */
	int cleanup_rc = logger_ctor_rollback(l, &state);
	(void)cleanup_rc;
	errno = primary;
	return NULL;
}
}

static void cancel_unreturned_logger(void *arg)
{
	logger_t *l = arg;
	/* scope_end cleared the TLS marker before cancellation became effective.
     * Dispose an object that never transferred to the caller. */
	(void)logger_dispose_internal(l);
}

/* Keep the ownership handoff separate from construction: cleanup macros may
 * implement setjmp scopes, whereas construction has many mutable resources. */
static logger_t *finish_create(logger_scope_t *scope, logger_t *l, int error)
{
	pthread_cleanup_push(cancel_unreturned_logger, l);
	(void)logger_scope_end(scope, error);
	if (scope->old_cancel == PTHREAD_CANCEL_ENABLE)
		pthread_testcancel();
	pthread_cleanup_pop(0);
	return l;
}

static logger_t *create_entry(const logger_config_t *input,
			      logger_file_t *reserved)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return NULL;
	/* Returning an owned pointer cannot be made an atomic handoff under
     * enabled asynchronous cancellation. Reject BEFORE acquiring resources.
     * Global/Audit callers enter with cancellation already disabled. */
	int old_type;
	int rc = pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type);
	if (!rc)
		rc = pthread_setcanceltype(old_type, NULL);
	if (rc || (old_type == PTHREAD_CANCEL_ASYNCHRONOUS &&
		   scope.old_cancel == PTHREAD_CANCEL_ENABLE)) {
		(void)logger_scope_end(&scope, rc ? -rc : -ENOTSUP);
		return NULL;
	}
	logger_t *l = create_logger(input, reserved);
	int error = l ? 0 : -(errno ? errno : EIO);
	/* Resolve a pending deferred cancellation before ownership handoff. The
     * cleanup is registered while disabled; no API CP remains after pop/return.
     * Callers still must install their own cleanup for later cancellation. */
	return finish_create(&scope, l, error);
}

logger_t *logger_create(const logger_config_t *input)
{
	return create_entry(input, NULL);
}

logger_t *logger_create_reserved_file(const logger_config_t *input,
				      logger_file_t *reserved)
{
	if (!reserved) {
		errno = EINVAL;
		return NULL;
	}
	return create_entry(input, reserved);
}

static int dispose_body(logger_t *l)
{
	if (!l)
		return 0;

	/* Linux lifetime rule: stop publication/execution first, prove every worker
	 * and synchronization user is gone, then release dependent storage. A
	 * lifecycle-proof failure intentionally leaks the partially retired object;
	 * freeing it would turn an invariant failure into a possible UAF. */
	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPING,
			      memory_order_release);

	if (l->async_mode) {
		atomic_store_explicit(&l->running, 0, memory_order_release);
		logger_queue_wake_force(&l->q);
		int join_rc = pthread_join(l->worker, NULL);
		if (join_rc) {
			int expected = 0;
			(void)atomic_compare_exchange_strong_explicit(
				&l->lifecycle_error, &expected, join_rc,
				memory_order_release, memory_order_relaxed);
			return -join_rc;
		}
	}

	int lifecycle_error = atomic_load_explicit(
		&l->lifecycle_error, memory_order_acquire);

	int lock_rc = __cleanup_lockdep_mutex_lock(&l->emit_mu, LOGGER_LOCK_INSTANCE_EMIT);
	if (lock_rc) {
		atomic_store_explicit(&l->lifecycle_error, lock_rc,
				      memory_order_release);
		return -lock_rc;
	}
	int rc = lifecycle_error ? -lifecycle_error :
		 logger_sync_outputs_locked(l);
	int unlock_rc = __cleanup_lockdep_mutex_unlock(&l->emit_mu, LOGGER_LOCK_INSTANCE_EMIT);
	if (unlock_rc) {
		atomic_store_explicit(&l->lifecycle_error, unlock_rc,
				      memory_order_release);
		return -unlock_rc;
	}

	/* Worker no longer exists, so its private workspace is no longer shared. */
	if (l->async_mode) {
		logger_worker_workspace_destroy(l->worker_workspace);
		l->worker_workspace = NULL;
		int qrc = logger_queue_destroy(&l->q);
		if (qrc) {
			atomic_store_explicit(&l->lifecycle_error, -qrc,
					      memory_order_release);
			return qrc;
		}
	}

	/* These destroys are lifetime assertions, not best-effort cleanup. */
	int destroy_rc = pthread_cond_destroy(&l->progress_cv);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}
	destroy_rc = pthread_mutex_destroy(&l->progress_mu);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}
	destroy_rc = pthread_mutex_destroy(&l->emit_mu);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}

	int close_rc = logger_file_close_status(&l->file_backend);
	if (!rc)
		rc = close_rc;
	if (l->syslog_backend.fd >= 0) {
		int fd = l->syslog_backend.fd;
		l->syslog_backend.fd = -1;
		if (close(fd) < 0 && !rc)
			rc = -errno;
	}

	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPED,
			      memory_order_release);

	/* The process census is an invariant gate. Underflow must not be hidden by
	 * freeing the object and pretending that no runtime exists. */
	int release_rc = logger_process_object_release();
	if (release_rc)
		return release_rc;

	free(l);
	return rc;
}

int logger_dispose_internal(logger_t *l)
{
	/* NULL destruction retains its no-registration/no-resource behavior. */
	if (!l)
		return reject_access() ? -errno : 0;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);
	if (rc)
		return rc;
	rc = dispose_body(l);
	return logger_scope_end(&scope, rc);
}

int logger_destroy_status(logger_t *l)
{
	int rc = logger_dispose_internal(l);
	if (rc) {
		errno = -rc;
		return -1;
	}
	return 0;
}

void logger_destroy(logger_t *l)
{
	(void)logger_destroy_status(l);
}

void logger_log(logger_t *l, logger_level_t level, const char *module,
		const char *file, int line, const char *function,
		const char *fmt, ...)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	int rc = !l || !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		vlog_body(l, level, module, file, line, function, fmt, ap);
		va_end(ap);
	}
	(void)logger_scope_end(&scope, rc);
}
void logger_log_source(logger_t *l, logger_level_t level,
		       logger_source_t source, const char *fmt, ...)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return;
	int rc = !l || !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		vlog_body(l, level, source.module, source.file, source.line,
			  source.func, fmt, ap);
		va_end(ap);
	}
	(void)logger_scope_end(&scope, rc);
}

int logger_log_sync_status(logger_t *l, logger_level_t level,
			   const char *module, const char *file, int line,
			   const char *function, const char *fmt, ...)
{
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);
	if (rc)
		return rc;
	if (!l || !fmt || (unsigned)level >= LOGGER_OFF)
		rc = -EINVAL;
	else if (atomic_load_explicit(&l->state, memory_order_acquire) !=
		 LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	else if (level >=
		 atomic_load_explicit(&l->level, memory_order_relaxed)) {
		logger_message_t msg = { .level = level };
		prepare_record_metadata(l, &msg, module, file, line, function);
		if (clock_gettime(CLOCK_REALTIME, &msg.ts))
			rc = -errno;
		else {
			va_list ap;
			va_start(ap, fmt);
			int n = vsnprintf(msg.text, sizeof(msg.text), fmt, ap);
			va_end(ap);
			if (n < 0)
				rc = -EILSEQ;
			else if ((size_t)n >= sizeof(msg.text))
				rc = -EOVERFLOW;
			else
				rc = logger_emit_status(l, &msg, 1);
		}
	}
	return logger_scope_end(&scope, rc);
}

int logger_reopen_instance(logger_t *l)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	int rc = !l || !(l->outputs & LOGGER_OUT_FILE) ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
		rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
		if (!rc) {
			rc = logger_file_reopen(&l->file_backend) ?
				     -(errno ? errno : EIO) :
				     0;
			if (rc)
				logger_note_io_error(l, -rc);
		}
	}
	return logger_scope_end(&scope, rc) ? -1 : 0;
}
int logger_file_offset(logger_t *l, uint64_t *out)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	int rc = !l || !out ? -EINVAL : 0;
	if (!rc) {
		ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
		rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
		if (!rc)
			rc = logger_file_offset_get(&l->file_backend, out) ?
				     -(errno ? errno : EIO) :
				     0;
	}
	return logger_scope_end(&scope, rc) ? -1 : 0;
}
void logger_note_io_error(logger_t *l, int error)
{
	if (error <= 0)
		return;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(&l->first_error,
						      &expected, error,
						      memory_order_release,
						      memory_order_relaxed);
}

int logger_wait_for_output(logger_t *l)
{
	if (logger_process_is_child())
		return -ECHILD;
	if (!l)
		return -EINVAL;
	if (l->async_mode) {
		/* 顺序由 reservation counter 定义，而不是 enqueued metrics。
		 * snapshot 前已经 reserve、但尚未 publish 的 slot 也必须计入；
		 * 慢 producer 不能被后续 record 的 completion 越过。 */
		size_t target = atomic_load_explicit(&l->q.enqueue_pos,
						     memory_order_acquire);
		ACQUIRE(pthread_mutex_progress_checked, progress_guard)(&l->progress_mu);
		int rc = ACQUIRE_ERR(pthread_mutex_progress_checked, &progress_guard);
		if (rc)
			return rc;
		while (l->completed_pos - target > SIZE_MAX / 2) {
			rc = pthread_cond_wait(&l->progress_cv,
					       &l->progress_mu);
			if (rc != 0)
				return -rc;
		}
	}
	int error = atomic_load_explicit(&l->first_error, memory_order_acquire);
	return error ? -error : 0;
}

int logger_sync_outputs_locked(logger_t *l)
{
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &l->emit_mu);
	if (l->outputs & LOGGER_OUT_FILE) {
		int rc = logger_file_sync(&l->file_backend);
		if (rc < 0)
			logger_note_io_error(l, -rc);
	}
	int error = atomic_load_explicit(&l->first_error, memory_order_acquire);
	return error ? -error : 0;
}

int logger_flush_instance_status(logger_t *l)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	int rc = !l ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		/* reservation->notification 与 wait->sync 整段都处于 cancellation
		 * disabled scope；等待 worker progress 时不能持有 emit_mu。 */
		rc = logger_wait_for_output(l);
		int sync_rc;
		{
			ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
			sync_rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
			if (!sync_rc)
				sync_rc = logger_sync_outputs_locked(l);
		}
		if (!rc)
			rc = sync_rc;
	}
	return logger_scope_end(&scope, rc) ? -1 : 0;
}

void logger_flush_instance(logger_t *l)
{
	if (l)
		(void)logger_flush_instance_status(l);
}

void logger_set_instance_level(logger_t *l, logger_level_t v)
{
	if (reject_access())
		return;
	if ((unsigned)v > LOGGER_OFF) {
		errno = EINVAL;
		return;
	}
	if (l)
		atomic_store_explicit(&l->level, v, memory_order_relaxed);
}
uint64_t logger_dropped(const logger_t *l)
{
	if (reject_access())
		return 0;
	if (!l)
		return 0;
	uint64_t n = 0;
	for (unsigned i = 0; i < 6; i++)
		n += atomic_load_explicit(&l->dropped_by_level[i],
					  memory_order_relaxed);
	return n;
}
void logger_get_metrics(const logger_t *l, logger_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (reject_access())
		return;
	if (!l)
		return;
	for (unsigned i = 0; i < 6; i++)
		out->dropped[i] = atomic_load_explicit(&l->dropped_by_level[i],
						       memory_order_relaxed);
	out->queue_high_watermark = atomic_load_explicit(
		&l->queue_high_watermark, memory_order_relaxed);
	out->enqueued =
		atomic_load_explicit(&l->enqueued, memory_order_relaxed);
	out->sync_fallbacks =
		atomic_load_explicit(&l->sync_fallbacks, memory_order_relaxed);
	out->consumer_batches = atomic_load_explicit(&l->consumer_batches,
						     memory_order_relaxed);
	out->consumer_records = atomic_load_explicit(&l->consumer_records,
						     memory_order_relaxed);
}

void logger_get_io_metrics(const logger_t *l, logger_io_metrics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (reject_access())
		return;
	if (!l)
		return;
	out->async_completed =
		atomic_load_explicit(&l->async_completed, memory_order_acquire);
	out->sync_completed =
		atomic_load_explicit(&l->sync_completed, memory_order_acquire);
	out->emitted_records =
		atomic_load_explicit(&l->emitted_records, memory_order_relaxed);
	out->failed_records =
		atomic_load_explicit(&l->failed_records, memory_order_relaxed);
	out->first_error =
		atomic_load_explicit(&l->first_error, memory_order_acquire);
}

void logger_get_diagnostics(const logger_t *l, logger_diagnostics_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (reject_access() || !l)
		return;

	out->state = atomic_load_explicit(&l->state, memory_order_acquire);
	out->level = atomic_load_explicit(&l->level, memory_order_relaxed);
	out->outputs = l->outputs;
	out->async_mode = l->async_mode;
	out->worker_running =
		l->async_mode ?
			atomic_load_explicit(&l->running, memory_order_acquire) :
			0;

	for (unsigned i = 0; i < 6; ++i)
		out->dropped_records += atomic_load_explicit(
			&l->dropped_by_level[i], memory_order_relaxed);
	out->sync_fallbacks =
		atomic_load_explicit(&l->sync_fallbacks, memory_order_relaxed);
	out->enqueued =
		atomic_load_explicit(&l->enqueued, memory_order_relaxed);
	out->consumer_batches =
		atomic_load_explicit(&l->consumer_batches, memory_order_relaxed);
	out->consumer_records =
		atomic_load_explicit(&l->consumer_records, memory_order_relaxed);
	out->async_completed =
		atomic_load_explicit(&l->async_completed, memory_order_acquire);
	out->sync_completed =
		atomic_load_explicit(&l->sync_completed, memory_order_acquire);
	out->emitted_records =
		atomic_load_explicit(&l->emitted_records, memory_order_relaxed);
	out->failed_records =
		atomic_load_explicit(&l->failed_records, memory_order_relaxed);
	out->first_error =
		atomic_load_explicit(&l->first_error, memory_order_acquire);

	if (l->async_mode) {
		out->queue_capacity = (uint64_t)l->q.cap;
		out->queue_depth = (uint64_t)logger_queue_depth(&l->q);
		out->queue_high_watermark = atomic_load_explicit(
			&l->queue_high_watermark, memory_order_relaxed);
		out->queue_storage_bytes =
			(uint64_t)logger_queue_storage_bytes(&l->q);
		out->spill_capacity =
			(uint64_t)logger_queue_spill_capacity(&l->q);
		out->spill_in_use =
			(uint64_t)logger_queue_spill_in_use(&l->q);
		out->spill_exhaustions =
			logger_queue_spill_exhaustions(&l->q);
		out->consumer_waiting = (int)atomic_load_explicit(
			&l->q.consumer_waiting, memory_order_acquire);
		out->worker_wait_count = logger_queue_wait_count(&l->q);
		out->producer_wake_signals =
			logger_queue_producer_wake_signals(&l->q);
		out->force_wake_signals =
			logger_queue_force_wake_signals(&l->q);
		out->completion_backlog =
			out->enqueued > out->async_completed ?
				out->enqueued - out->async_completed :
				0;
	}

	if (out->dropped_records)
		out->observation_flags |= LOGGER_DIAG_DROPS_OBSERVED;
	if (out->sync_fallbacks)
		out->observation_flags |= LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED;
	if (out->queue_capacity &&
	    out->queue_high_watermark >= out->queue_capacity)
		out->observation_flags |= LOGGER_DIAG_QUEUE_SATURATED;
	if (out->spill_exhaustions)
		out->observation_flags |=
			LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED;
	if (out->failed_records || out->first_error)
		out->observation_flags |= LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED;
	if (out->async_mode && out->state == LOGGER_STATE_RUNNING &&
	    !out->worker_running)
		out->observation_flags |= LOGGER_DIAG_WORKER_STOPPED;
}

int logger_file_metrics_snapshot(logger_t *l, logger_file_metrics_t *out)
{
	if (!l || !out)
		return -EINVAL;
	if (!(l->outputs & LOGGER_OUT_FILE))
		return -ENOTSUP;
	ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
	int rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
	if (rc)
		return rc;
	logger_file_metrics_t snapshot = l->file_backend.metrics;
	snapshot.current_size = (uint64_t)l->file_backend.current_size;
	snapshot.detached = l->file_backend.detached;
	*out = snapshot;
	return 0;
}

int logger_get_file_metrics(logger_t *l, logger_file_metrics_t *out)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	int rc = logger_file_metrics_snapshot(l, out);
	return logger_scope_end(&scope, rc) ? -1 : 0;
}

int logger_get_syslog_metrics(logger_t *l, logger_syslog_metrics_t *out)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return -1;
	int rc = !l || !out ? -EINVAL : 0;
	if (!rc && !(l->outputs & LOGGER_OUT_SYSLOG))
		rc = -ENOTSUP;
	if (!rc) {
		ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
		rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
		if (!rc)
			*out = l->syslog_backend.metrics;
	}
	return logger_scope_end(&scope, rc) ? -1 : 0;
}
