#define _GNU_SOURCE
#include "audit.h"
#include "audit_internal.h"
#include "audit_record.h"
#include "logger_internal.h"
#include "logger_fault.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/* Audit already serializes every durable record. One operation mutex protects
 * the published session, including its sequence and hash. The control mutex
 * serializes init, shutdown and disposal, but is NOT acquired by writers.
 * Lock order: control -> operation. No internal helper calls public APIs.
 * Admission closes before shutdown waits for operation, preventing new work
 * from extending the old session. Blocked calls recheck admission under lock.
 */
typedef struct {
	logger_t *logger;
	int writer_lock_fd;
	int log_dir_fd;
	logger_file_t reserved_log;
	logger_file_t checkpoint_owner;
	char state_path[AUDIT_PATH_MAX];
	char instance_id[33];
	audit_integrity_t integrity;
	const audit_digest_ops_t *digest;
	audit_state_t health;
	int error;
	uint64_t seq;
	uint64_t next_txn;
	uint64_t checkpoint_seq;
	unsigned char head[32];
	audit_ckpt_t pending;
	int checkpoint_dirty;
	int offset_valid;
} audit_runtime_t;

static pthread_mutex_t g_control_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_operation_mu = PTHREAD_MUTEX_INITIALIZER;
static audit_runtime_t *g_runtime; /* operation mutex; never exposed to callers */
static audit_status_t g_last_status;
static _Atomic int g_phase = AUDIT_STATE_IDLE;
/* Reject a call delayed across shutdown/reinit, not merely while STOPPING. */
static _Atomic uint64_t g_generation;
static _Atomic int g_policy = AUDIT_FAIL_REPORT;
static int result(int rc)
{
	if (rc < 0) {
		errno = -rc;
		return -1;
	}
	return 0;
}

/* Cancellation is deferred until all owned locks/resources have been released.
 * Cancellation is not an application-level rollback of a committed record. */
static int lock_scope(pthread_mutex_t *mu, int *old_cancel)
{
	if (logger_scope_busy())
		return -EDEADLK;
	int rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, old_cancel);
	if (rc)
		return -rc;
	rc = pthread_mutex_lock(mu);
	if (rc)
		(void)pthread_setcancelstate(*old_cancel, NULL);
	return -rc;
}

static void unlock_scope(pthread_mutex_t *mu, int old_cancel)
{
	(void)pthread_mutex_unlock(mu);
	(void)pthread_setcancelstate(old_cancel, NULL);
}

static int admission_error(void)
{
	if (logger_process_is_child())
		return ECHILD;
	switch (atomic_load_explicit(&g_phase, memory_order_acquire)) {
	case AUDIT_STATE_RUNNING:
		return 0;
	case AUDIT_STATE_STARTING:
		return EAGAIN;
	case AUDIT_STATE_STOPPING:
		return ESHUTDOWN;
	default:
		return ENODEV;
	}
}

static int lock_runtime(audit_runtime_t **runtime, int *old_cancel)
{
	uint64_t generation =
		atomic_load_explicit(&g_generation, memory_order_acquire);
	int error = admission_error();
	if (error)
		return -error;
	int rc = lock_scope(&g_operation_mu, old_cancel);
	if (rc)
		return rc;
	error = admission_error();
	if (!error && generation != atomic_load_explicit(&g_generation,
							 memory_order_relaxed))
		error = ESHUTDOWN;
	if (!error && !g_runtime)
		error = ENODEV;
	if (error) {
		unlock_scope(&g_operation_mu, *old_cancel);
		return -error;
	}
	*runtime = g_runtime;
	return 0;
}

static int generate_instance_id(char out[33])
{
	unsigned char raw[16];
	size_t off = 0;
	while (off < sizeof(raw)) {
		ssize_t n = getrandom(raw + off, sizeof(raw) - off, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!n)
			return -EIO;
		off += (size_t)n;
	}
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(raw); ++i) {
		out[i * 2] = hex[raw[i] >> 4];
		out[i * 2 + 1] = hex[raw[i] & 15];
	}
	out[32] = '\0';
	return 0;
}

static int acquire_writer_lock(audit_runtime_t *s, const char *dir,
			       const char *name)
{
	char path[AUDIT_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s.audit.lock", dir, name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	struct stat st;
	int error = 0;
	if (fstat(fd, &st))
		error = errno;
	else if (!S_ISREG(st.st_mode))
		error = EINVAL;
	if (!error) {
		struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
		if (fcntl(fd, F_SETLK, &lock) < 0)
			error = (errno == EACCES || errno == EAGAIN) ? EBUSY :
								       errno;
	}
	if (error) {
		(void)close(fd);
		return -error;
	}
	s->writer_lock_fd = fd;
	return 0;
}

static int dispose_runtime(audit_runtime_t *s)
{
	if (!s)
		return 0;
	/* The session retains writer ownership until all output teardown finishes.
     * Do not unlink the lock file: other processes coordinate on this inode. */
	int rc = logger_destroy_status(s->logger) ? -errno : 0;
	int other = logger_file_close_status(&s->reserved_log);
	if (!rc)
		rc = other;
	other = logger_file_close_status(&s->checkpoint_owner);
	if (!rc)
		rc = other;
	if (s->writer_lock_fd >= 0 && close(s->writer_lock_fd) < 0 && !rc)
		rc = -errno;
	if (s->log_dir_fd >= 0 && close(s->log_dir_fd) < 0 && !rc)
		rc = -errno;
	free(s);
	return rc;
}

static int safe_name(const char *s)
{
	if (!s || !*s)
		return 0;
	for (; *s; ++s)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
		      (*s >= 'A' && *s <= 'Z') || *s == '_' || *s == '-' ||
		      *s == '.'))
			return 0;
	return 1;
}

static int validate_config(const audit_config_t *c)
{
	if (!c)
		return -EINVAL;
	if (!(c->struct_size == 0 && c->version == 0)) {
		if (c->version != AUDIT_CONFIG_VERSION)
			return -EPROTONOSUPPORT;
		if (c->struct_size < sizeof(*c))
			return -EINVAL;
	}
	if (!c->log_dir || !safe_name(c->name) ||
	    (unsigned)c->failure_policy > AUDIT_FAIL_DENY)
		return -EINVAL;
	if (c->integrity != AUDIT_INTEGRITY_NONE &&
	    !audit_digest_provider(c->integrity))
		return -EPROTONOSUPPORT;
	return 0;
}

static int validate_event(const audit_event_t *e)
{
	if (!e || !e->event || !*e->event || !e->operation || !*e->operation ||
	    (unsigned)e->phase > AUDIT_PHASE_RESULT ||
	    (e->phase == AUDIT_PHASE_RESULT &&
	     (unsigned)e->result > AUDIT_FAILURE))
		return -EINVAL;
	/* These lifecycle markers must not be forgeable through the public API. */
	if (!strcmp(e->event, "AUDIT_START") || !strcmp(e->event, "AUDIT_STOP"))
		return -EINVAL;
	return 0;
}

static int fail_runtime(audit_runtime_t *s, audit_state_t state, int rc);

static int encode_event(audit_runtime_t *s, const audit_event_t *e,
			uint64_t seq, char *out, size_t cap,
			unsigned char hash[32])
{
	/* Reserve both chain fields BEFORE hashing so overflow is an input error,
     * never a crypto/backend failure. Encoder/parser share one KV grammar. */
	size_t suffix = s->digest ? 140u : 0u;
	if (cap <= suffix)
		return -EOVERFLOW;
	size_t used;
	int encoded = audit_payload_encode(s->instance_id, seq, e, out,
					   cap - suffix, &used);
	if (encoded)
		return encoded;
	int n;
	if (s->digest) {
		char prev[65], current[65];
		audit_hash_hex(s->head, prev);
		n = snprintf(out + used, cap - used, " prev=%s", prev);
		if (n < 0 || (size_t)n >= cap - used)
			return -EOVERFLOW;
		used += (size_t)n;
		int rc = s->digest->hash(out, used, hash);
		if (rc)
			return fail_runtime(s, AUDIT_STATE_CRYPTO_FAILED, rc);
		audit_hash_hex(hash, current);
		n = snprintf(out + used, cap - used, " hash=%s", current);
		if (n < 0 || (size_t)n >= cap - used)
			return -EOVERFLOW;
	}
	return 0;
}

static int fail_runtime(audit_runtime_t *s, audit_state_t state, int rc)
{
	s->health = state;
	s->error = rc < 0 ? -rc : EIO;
	return -s->error;
}

static int checkpoint_runtime(audit_runtime_t *s)
{
	if (!s->checkpoint_dirty)
		return 0;
	if (!s->offset_valid) {
		if (logger_file_offset(s->logger, &s->pending.offset))
			return fail_runtime(s, AUDIT_STATE_CHECKPOINT_FAILED,
					    -errno);
		s->offset_valid = 1;
	}
	if (audit_checkpoint_persist(s->state_path, &s->pending))
		return fail_runtime(s, AUDIT_STATE_CHECKPOINT_FAILED, -errno);
	logger_fault_crash_if_requested("after_checkpoint_commit");
	s->checkpoint_seq = s->seq;
	s->checkpoint_dirty = 0;
	s->health = AUDIT_STATE_RUNNING;
	s->error = 0;
	return 0;
}

/* Called either on a hidden candidate owned by init, or under operation_mu.
 * Validation/capacity failures have no output effects and do not poison the
 * session. Crypto failure also precedes output, but latches CRYPTO_FAILED until
 * shutdown/reinit: it is not a business input error. Backend errors are uncertain.
 */
static int write_runtime(audit_runtime_t *s, const audit_event_t *event)
{
	if (s->health != AUDIT_STATE_RUNNING)
		return -(s->error ? s->error : EIO);
	if (s->seq == UINT64_MAX)
		return -EOVERFLOW;
	uint64_t next = s->seq + 1;
	_Static_assert(AUDIT_PAYLOAD_MAX + 1u == LOGGER_MESSAGE_MAX,
		       "audit payload bound");
	char line[AUDIT_PAYLOAD_MAX + 1u];
	unsigned char hash[32] = { 0 };
	int rc = encode_event(s, event, next, line, sizeof(line), hash);
	if (rc)
		return rc;
	rc = logger_log_sync_status(s->logger, LOGGER_INFO, "AUDIT", NULL, 0,
				    NULL, "%s", line);
	if (rc)
		return fail_runtime(s, AUDIT_STATE_IO_FAILED, rc);

	/* Record commit precedes checkpoint commit. Never leave the in-memory head
     * behind a confirmed log append, even when offset/checkpoint work fails. */
	s->seq = next;
	if (s->digest) {
		memcpy(s->head, hash, sizeof(s->head));
		s->pending =
			(audit_ckpt_t){ .algorithm = (uint32_t)s->integrity,
					.seq = next };
		memcpy(s->pending.hash, hash, sizeof(hash));
		s->checkpoint_dirty = 1;
		s->offset_valid = 0;
	}
	logger_fault_crash_if_requested("after_audit_fsync");
	return checkpoint_runtime(s);
}

static void snapshot_runtime(const audit_runtime_t *s, audit_status_t *out)
{
	*out = (audit_status_t){ .state = s->health,
				 .error_code = s->error,
				 .checkpoint_dirty = s->checkpoint_dirty,
				 .committed_seq = s->seq,
				 .checkpoint_seq = s->checkpoint_seq };
	memcpy(out->instance_id, s->instance_id, sizeof(out->instance_id));
}

static int create_runtime(const audit_config_t *c, audit_runtime_t **out)
{
	audit_runtime_t *s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	*out = s; /* caller owns cleanup on every subsequent error */
	s->writer_lock_fd = -1;
	s->log_dir_fd = -1;
	s->reserved_log = LOGGER_FILE_EMPTY;
	s->checkpoint_owner = LOGGER_FILE_EMPTY;
	s->integrity = c->integrity;
	s->digest = audit_digest_provider(c->integrity);
	s->health = AUDIT_STATE_RUNNING;
	int rc = s->digest ? audit_digest_check(s->digest) : 0;
	if (rc)
		return rc; /* before lock creation, recovery, checkpoint or logger open */
	rc = generate_instance_id(s->instance_id);
	if (rc)
		return rc;
	char path[AUDIT_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s.audit.log", c->log_dir,
			 c->name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;
	if (c->chain_state_path)
		n = snprintf(s->state_path, sizeof(s->state_path), "%s",
			     c->chain_state_path);
	else
		n = snprintf(s->state_path, sizeof(s->state_path),
			     "%s/%s.audit.state", c->log_dir, c->name);
	if (n < 0 || (size_t)n >= sizeof(s->state_path))
		return -ENAMETOOLONG;
	/* Lock the ordinary file target BEFORE any recovery can inspect or
     * truncate it. Recovery keeps its path API, anchored via Linux procfd
     * names to directory descriptors we own for the session lifetime. This
     * adds no process scanning or fork policy. */
	if (logger_file_reserve(&s->reserved_log, path, c->rotation, 0600))
		return -errno;
	if (!s->reserved_log.managed)
		return -EINVAL;
	s->log_dir_fd = fcntl(s->reserved_log.dir_fd, F_DUPFD_CLOEXEC, 0);
	if (s->log_dir_fd < 0)
		return -errno;
	char directory[64];
	n = snprintf(directory, sizeof(directory), "/proc/self/fd/%d",
		     s->log_dir_fd);
	if (n < 0 || (size_t)n >= sizeof(directory))
		return -ENAMETOOLONG;
	/* Fail before recovery if procfs fd traversal is unavailable. */
	struct stat bound;
	if (stat(directory, &bound))
		return -errno;
	rc = acquire_writer_lock(s, directory, c->name);
	if (rc)
		return rc;
	n = snprintf(path, sizeof(path), "%s/%s", directory,
		     s->reserved_log.name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;
	if (s->digest) {
		if (!c->chain_state_path) {
			n = snprintf(s->state_path, sizeof(s->state_path),
				     "%s/%s.audit.state", directory, c->name);
			if (n < 0 || (size_t)n >= sizeof(s->state_path))
				return -ENAMETOOLONG;
		}
		logger_rotation_config_t none = { .mode = LOGGER_ROTATE_NONE };
		if (logger_file_reserve(&s->checkpoint_owner, s->state_path,
					none, 0600))
			return -errno;
		if (!s->checkpoint_owner.managed)
			return -EINVAL;
		n = snprintf(s->state_path, sizeof(s->state_path),
			     "/proc/self/fd/%d/%s", s->checkpoint_owner.dir_fd,
			     s->checkpoint_owner.name);
		if (n < 0 || (size_t)n >= sizeof(s->state_path))
			return -ENAMETOOLONG;
		audit_ckpt_t checkpoint;
		if (audit_checkpoint_load(s->state_path, &checkpoint))
			return -errno;
		if (!checkpoint.algorithm)
			checkpoint.algorithm = (uint32_t)c->integrity;
		if (checkpoint.algorithm != (uint32_t)c->integrity)
			return -EPROTONOSUPPORT;
		if (audit_recover_set(directory, c->name, path, &checkpoint) ||
		    audit_checkpoint_persist(s->state_path, &checkpoint))
			return -errno;
		memcpy(s->head, checkpoint.hash, sizeof(s->head));
	}
	logger_config_t lc = LOGGER_DEFAULT_CONFIG();
	lc.level = LOGGER_INFO;
	lc.outputs = LOGGER_OUT_FILE;
	lc.file_path = path;
	lc.file_mode = 0600;
	lc.rotation = c->rotation;
	lc.async_mode = 0;
	lc.include_source = 0;
	lc.ident = "audit";
	/* Strict audit always uses the synchronous, forced-sync backend API. */
	s->logger = logger_create_reserved_file(&lc, &s->reserved_log);
	if (!s->logger)
		return -errno;
	audit_event_t start = { .phase = AUDIT_PHASE_RESULT,
				.event = "AUDIT_START",
				.actor = "system",
				.source = "local",
				.resource = c->name,
				.operation = "audit_start",
				.result = AUDIT_SUCCESS };
	return write_runtime(s, &start);
}

int audit_init(const audit_config_t *c)
{
	if (logger_process_is_child())
		return result(-ECHILD);
	int rc = logger_process_ensure();
	if (rc)
		return result(rc);
	int old_cancel;
	rc = lock_scope(&g_control_mu, &old_cancel);
	if (rc)
		return result(rc);
	pthread_mutex_lock(&g_operation_mu);
	if (g_runtime) {
		pthread_mutex_unlock(&g_operation_mu);
		unlock_scope(&g_control_mu, old_cancel);
		return result(-EALREADY);
	}
	if (atomic_load_explicit(&g_generation, memory_order_relaxed) ==
	    UINT64_MAX) {
		pthread_mutex_unlock(&g_operation_mu);
		unlock_scope(&g_control_mu, old_cancel);
		return result(-EOVERFLOW);
	}
	atomic_store_explicit(&g_phase, AUDIT_STATE_STARTING,
			      memory_order_release);
	atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
	pthread_mutex_unlock(&g_operation_mu);

	audit_runtime_t *candidate = NULL;
	rc = validate_config(c);
	if (!rc)
		rc = create_runtime(c, &candidate);
	if (!rc) {
		pthread_mutex_lock(&g_operation_mu);
		g_runtime =
			candidate; /* START and its checkpoint have succeeded */
		atomic_store_explicit(&g_policy, c->failure_policy,
				      memory_order_relaxed);
		atomic_store_explicit(&g_phase, AUDIT_STATE_RUNNING,
				      memory_order_release);
		pthread_mutex_unlock(&g_operation_mu);
	} else {
		audit_status_t failed = { 0 };
		if (candidate)
			snapshot_runtime(candidate, &failed);
		/* Failed initialization never emits a fake STOP. Keep the original
         * failure even if cleanup hits another error. */
		(void)dispose_runtime(candidate);
		failed.state = AUDIT_STATE_IDLE;
		failed.error_code = -rc;
		pthread_mutex_lock(&g_operation_mu);
		g_last_status = failed;
		atomic_store_explicit(&g_phase, AUDIT_STATE_IDLE,
				      memory_order_release);
		pthread_mutex_unlock(&g_operation_mu);
	}
	unlock_scope(&g_control_mu, old_cancel);
	return result(rc);
}

void audit_after_fork_child(void)
{
	/* Compatibility entry point, now invalidation only. A forked child must
     * exec before reinitializing Audit. CLOEXEC descriptors close on exec;
     * do not reset inherited mutexes, free runtime, or attempt an AUDIT_STOP. */
	logger_process_invalidate_child();
}

int audit_shutdown_status(void)
{
	int ready = logger_process_ensure();
	if (ready)
		return result(ready);
	int old_cancel;
	int rc = lock_scope(&g_control_mu, &old_cancel);
	if (rc)
		return result(rc);
	atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
			      memory_order_release);
	pthread_mutex_lock(
		&g_operation_mu); /* drains the in-flight operation */
	audit_runtime_t *s = g_runtime;
	g_runtime = NULL;
	audit_status_t final = g_last_status;
	rc = 0;
	if (s) {
		if (s->health == AUDIT_STATE_IO_FAILED ||
		    s->health == AUDIT_STATE_CRYPTO_FAILED)
			rc = -s->error; /* uncertain I/O or failed crypto: no fake STOP/resume */
		else
			rc = checkpoint_runtime(s);
		if (!rc) {
			audit_event_t stop = { .phase = AUDIT_PHASE_RESULT,
					       .event = "AUDIT_STOP",
					       .actor = "system",
					       .source = "local",
					       .resource = "audit",
					       .operation = "audit_stop",
					       .result = AUDIT_SUCCESS };
			rc = write_runtime(s, &stop);
		}
		snapshot_runtime(s, &final);
	}
	pthread_mutex_unlock(&g_operation_mu);
	int close_rc = dispose_runtime(s); /* still holding the control mutex */
	if (!rc)
		rc = close_rc;
	final.state = AUDIT_STATE_IDLE;
	if (rc)
		final.error_code = -rc;
	pthread_mutex_lock(&g_operation_mu);
	g_last_status = final;
	atomic_store_explicit(&g_phase, AUDIT_STATE_IDLE, memory_order_release);
	pthread_mutex_unlock(&g_operation_mu);
	unlock_scope(&g_control_mu, old_cancel);
	return result(rc);
}

void audit_shutdown(void)
{
	(void)audit_shutdown_status();
}

int audit_write(const audit_event_t *e)
{
	if (logger_process_is_child())
		return result(-ECHILD);
	int rc = validate_event(e);
	if (rc)
		return result(rc);
	audit_runtime_t *s;
	int old_cancel;
	rc = lock_runtime(&s, &old_cancel);
	if (!rc) {
		rc = write_runtime(s, e);
		unlock_scope(&g_operation_mu, old_cancel);
	}
	return result(rc);
}

int audit_begin(audit_event_t *e)
{
	if (logger_process_is_child())
		return result(-ECHILD);
	if (!e)
		return result(-EINVAL);
	audit_event_t next = *e;
	next.phase = AUDIT_PHASE_ATTEMPT;
	int rc = validate_event(&next);
	if (rc)
		return result(rc);
	audit_runtime_t *s;
	int old_cancel;
	rc = lock_runtime(&s, &old_cancel);
	if (rc)
		return result(rc);
	if (s->health != AUDIT_STATE_RUNNING)
		rc = -s->error;
	else if (!next.transaction_id && s->next_txn == UINT64_MAX)
		rc = -EOVERFLOW;
	else {
		if (!next.transaction_id)
			next.transaction_id = ++s->next_txn;
		*e = next;
		rc = write_runtime(s, &next);
	}
	unlock_scope(&g_operation_mu, old_cancel);
	return result(rc);
}

int audit_end(audit_event_t *e, audit_result_t outcome, int error_code)
{
	if (logger_process_is_child())
		return result(-ECHILD);
	if (!e || !e->transaction_id)
		return result(-EINVAL);
	audit_event_t next = *e;
	next.phase = AUDIT_PHASE_RESULT;
	next.result = outcome;
	next.error_code = error_code;
	int rc = validate_event(&next);
	if (rc)
		return result(rc);
	audit_runtime_t *s;
	int old_cancel;
	rc = lock_runtime(&s, &old_cancel);
	if (rc)
		return result(rc);
	if (s->health != AUDIT_STATE_RUNNING)
		rc = -s->error;
	else {
		*e = next;
		rc = write_runtime(s, &next);
	}
	unlock_scope(&g_operation_mu, old_cancel);
	return result(rc);
}

int audit_flush(void)
{
	audit_runtime_t *s;
	int old_cancel;
	int rc = lock_runtime(&s, &old_cancel);
	if (rc)
		return result(rc);
	if (s->health == AUDIT_STATE_IO_FAILED ||
	    s->health == AUDIT_STATE_CRYPTO_FAILED)
		rc = -s->error;
	else if (s->checkpoint_dirty)
		rc = checkpoint_runtime(
			s); /* no re-append; repair the committed head */
	else if (logger_flush_instance_status(s->logger))
		rc = fail_runtime(s, AUDIT_STATE_IO_FAILED, -errno);
	unlock_scope(&g_operation_mu, old_cancel);
	return result(rc);
}

int audit_get_status(audit_status_t *out)
{
	if (!out)
		return result(-EINVAL);
	*out = (audit_status_t){ 0 };
	if (logger_process_is_child()) {
		out->state = AUDIT_STATE_FORKED;
		out->error_code = ECHILD;
		return 0;
	}
	int ready = logger_process_ensure();
	if (ready)
		return result(ready);
	int phase = atomic_load_explicit(&g_phase, memory_order_acquire);
	if (phase == AUDIT_STATE_STARTING || phase == AUDIT_STATE_STOPPING) {
		out->state = (audit_state_t)phase;
		return 0; /* no partial candidate/session data is exposed */
	}
	int old_cancel;
	int rc = lock_scope(&g_operation_mu, &old_cancel);
	if (rc)
		return result(rc);
	phase = atomic_load_explicit(&g_phase, memory_order_acquire);
	if (phase == AUDIT_STATE_RUNNING && g_runtime)
		snapshot_runtime(g_runtime, out);
	else if (phase == AUDIT_STATE_IDLE)
		*out = g_last_status;
	else
		out->state = (audit_state_t)phase;
	unlock_scope(&g_operation_mu, old_cancel);
	return 0;
}

audit_failure_policy_t audit_failure_policy(void)
{
	if (logger_process_is_child())
		return AUDIT_FAIL_DENY;
	return (audit_failure_policy_t)atomic_load_explicit(
		&g_policy, memory_order_relaxed);
}

int audit_instance_id_copy(char out[33])
{
	if (!out)
		return result(-EINVAL);
	out[0] = '\0';
	audit_runtime_t *s;
	int old_cancel;
	int rc = lock_runtime(&s, &old_cancel);
	if (!rc) {
		memcpy(out, s->instance_id, 33);
		unlock_scope(&g_operation_mu, old_cancel);
	}
	return result(rc);
}

const char *audit_instance_id(void)
{
	static _Thread_local char copy[33];
	(void)audit_instance_id_copy(copy);
	return copy;
}
