#define _GNU_SOURCE
#include "audit.h"
#include "audit_internal.h"
#include "audit_record.h"
#include "logger_internal.h"
#include "logger_lockdep.h"
#include "logger_fault.h"
#include "logger_cleanup.h"
#include "logger_uapi.h"

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
	logger_file_t reserved_log;
	logger_file_t checkpoint_owner;
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
static _Atomic int g_operation_error;
static _Atomic int g_control_error;
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
static int lock_scope(pthread_mutex_t *mu, logger_lock_class_t class_id,
		      int *old_cancel)
{
	if (logger_scope_busy())
		return -EDEADLK;
	int lock_error = class_id == LOGGER_LOCK_AUDIT_OPERATION ?
			 atomic_load_explicit(&g_operation_error,
					      memory_order_acquire) :
			 atomic_load_explicit(&g_control_error,
					      memory_order_acquire);
	if (lock_error)
		return -lock_error;
	int rc = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, old_cancel);
	if (rc)
		return -rc;
	rc = __cleanup_lockdep_mutex_lock(mu, class_id);
	if (rc) {
		(void)pthread_setcancelstate(*old_cancel, NULL);
		return -rc;
	}
	return 0;
}

static int unlock_scope(pthread_mutex_t *mu, logger_lock_class_t class_id,
			int old_cancel)
{
	int rc = __cleanup_lockdep_mutex_unlock(mu, class_id);
	if (rc) {
		_Atomic int *error = class_id == LOGGER_LOCK_AUDIT_OPERATION ?
				     &g_operation_error :
				     &g_control_error;
		int expected = 0;
		(void)atomic_compare_exchange_strong_explicit(
			error, &expected, rc, memory_order_release,
			memory_order_relaxed);
		atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
				      memory_order_release);
		/* 所有权不确定时不恢复可取消状态，以免取消穿过仍持有的锁。 */
		return -rc;
	}
	if (class_id == LOGGER_LOCK_AUDIT_CONTROL &&
	    atomic_load_explicit(&g_operation_error, memory_order_acquire)) {
		/* control 锁虽已释放，operation 锁解锁失败后仍可能由当前线程持有；
		 * 保持取消禁用，避免待处理取消让线程带锁退出。 */
		return -atomic_load_explicit(&g_operation_error,
					     memory_order_relaxed);
	}
	rc = pthread_setcancelstate(old_cancel, NULL);
	return rc ? -rc : 0;
}

static int operation_lock_nested(void)
{
	/* 调用者持有 control；失败时不登记锁，也不得读取 g_runtime。 */
	int error = atomic_load_explicit(&g_operation_error,
					memory_order_acquire);
	if (error)
		return -error;
	int rc = __cleanup_lockdep_mutex_lock(
		&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION);
	if (rc)
		return -rc;
	return 0;
}

static int operation_unlock_nested(void)
{
	/* 解锁失败后状态不确定；封闭 Audit，保留 runtime 且拒绝后续锁操作。 */
	int rc = __cleanup_lockdep_mutex_unlock(
		&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION);
	if (rc) {
		int expected = 0;
		(void)atomic_compare_exchange_strong_explicit(
			&g_operation_error, &expected, rc, memory_order_release,
			memory_order_relaxed);
		atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
				      memory_order_release);
		return -rc;
	}
	return 0;
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
	int rc = lock_scope(&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			    old_cancel);
	if (rc)
		return rc;
	error = admission_error();
	if (!error && generation != atomic_load_explicit(&g_generation,
							 memory_order_relaxed))
		error = ESHUTDOWN;
	if (!error && !g_runtime)
		error = ENODEV;
	if (error) {
		int unlock_rc = unlock_scope(
			&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			*old_cancel);
		if (unlock_rc)
			return unlock_rc;
		return -error;
	}
	logger_lockdep_assert_held(LOGGER_LOCK_AUDIT_OPERATION,
				   &g_operation_mu);
	*runtime = g_runtime;
	return 0;
}

static int generate_instance_id(char out[33])
{
	unsigned char raw[16];
	size_t off = 0;
	while (off < sizeof(raw)) {
		ssize_t n = getrandom(raw + off, sizeof(raw) - off, GRND_NONBLOCK);
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

static int dispose_runtime(audit_runtime_t *s)
{
	if (!s)
		return 0;
	/* The active/state logger_file reservations retain cooperative ownership
     * until all output teardown finishes. */
	int rc = logger_destroy_status(s->logger) ? -errno : 0;
	int other = logger_file_close_status(&s->reserved_log);
	if (!rc)
		rc = other;
	other = logger_file_close_status(&s->checkpoint_owner);
	if (!rc)
		rc = other;
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

static int copy_audit_config(const audit_config_t *in,
			     audit_config_t *out)
{
	_Static_assert(AUDIT_CONFIG_SIZE == sizeof(*out),
		       "Audit v1 config layout changed");
	if (!in || !out)
		return -EINVAL;

	uint32_t size, version;
	memcpy(&size, (const unsigned char *)in, sizeof(size));
	memcpy(&version, (const unsigned char *)in + sizeof(size),
	       sizeof(version));
	int rc = logger_uapi_copy_struct(out, sizeof(*out), in, size, version,
					AUDIT_CONFIG_VERSION);
	if (rc)
		return rc;

	out->struct_size = sizeof(*out);
	out->version = AUDIT_CONFIG_VERSION;
	return 0;
}

static int validate_config(const audit_config_t *c)
{
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
	if (audit_checkpoint_persist_at(s->checkpoint_owner.dir_fd,
				       s->checkpoint_owner.name, &s->pending))
		return fail_runtime(s, AUDIT_STATE_CHECKPOINT_FAILED, -errno);
	logger_fault_crash_if_requested("after_checkpoint_commit");
	s->checkpoint_seq = s->seq;
	s->checkpoint_dirty = 0;
	s->health = AUDIT_STATE_RUNNING;
	s->error = 0;
	return 0;
}

/* The submission state is an internal receipt, not a new public ABI.
 * NOT_SUBMITTED: encoding/validation failed before the backend was entered.
 * UNCERTAIN: backend was entered but strict sync was not confirmed.
 * COMMITTED: strict log append succeeded, regardless of checkpoint outcome.
 */
typedef enum {
	AUDIT_NOT_SUBMITTED = 0,
	AUDIT_SUBMISSION_UNCERTAIN,
	AUDIT_RECORD_COMMITTED
} audit_submission_state_t;

/* Called either on a hidden candidate owned by init, or under operation_mu.
 * Validation/capacity failures have no output effects and do not poison the
 * session. Crypto failure also precedes output, but latches CRYPTO_FAILED until
 * shutdown/reinit: it is not a business input error. Backend errors are uncertain.
 */
static int write_runtime(audit_runtime_t *s, const audit_event_t *event,
			 audit_submission_state_t *submission)
{
	if (submission)
		*submission = AUDIT_NOT_SUBMITTED;
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
	/* Once we call the synchronous backend, an error may follow a partial
	 * or complete write. Callers must not treat the candidate as unissued. */
	if (submission)
		*submission = AUDIT_SUBMISSION_UNCERTAIN;
	rc = logger_log_sync_status(s->logger, LOGGER_INFO, "AUDIT", NULL, 0,
				    NULL, "%s", line);
	if (rc)
		return fail_runtime(s, AUDIT_STATE_IO_FAILED, rc);
	if (submission)
		*submission = AUDIT_RECORD_COMMITTED;

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
	char path[AUDIT_PATH_MAX], state_path[AUDIT_PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s.audit.log", c->log_dir,
			 c->name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -ENAMETOOLONG;
	if (c->chain_state_path)
		n = snprintf(state_path, sizeof(state_path), "%s",
			     c->chain_state_path);
	else
		n = snprintf(state_path, sizeof(state_path),
			     "%s/%s.audit.state", c->log_dir, c->name);
	if (n < 0 || (size_t)n >= sizeof(state_path))
		return -ENAMETOOLONG;

	/* Acquire the active file-backend owner BEFORE recovery. The reservation
	 * already contains the stable directory fd and basename; recovery never
	 * converts them back into synthetic pathnames. */
	if (logger_file_reserve(&s->reserved_log, path, c->rotation, 0600))
		return -errno;
	if (!s->reserved_log.managed)
		return -EINVAL;

	if (s->digest) {
		logger_rotation_config_t none = { .mode = LOGGER_ROTATE_NONE };
		if (logger_file_reserve(&s->checkpoint_owner, state_path,
					none, 0600))
			return -errno;
		if (!s->checkpoint_owner.managed)
			return -EINVAL;

		audit_ckpt_t checkpoint;
		if (audit_checkpoint_load_at(s->checkpoint_owner.dir_fd,
					     s->checkpoint_owner.name, &checkpoint))
			return -errno;
		if (!checkpoint.algorithm)
			checkpoint.algorithm = (uint32_t)c->integrity;
		if (checkpoint.algorithm != (uint32_t)c->integrity)
			return -EPROTONOSUPPORT;
		if (audit_recover_set_at(s->reserved_log.dir_fd, c->name,
					 s->reserved_log.name, &checkpoint) ||
		    audit_checkpoint_persist_at(s->checkpoint_owner.dir_fd,
						s->checkpoint_owner.name,
						&checkpoint))
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
	return write_runtime(s, &start, NULL);
}

int audit_init(const audit_config_t *c)
{
	if (logger_process_is_child())
		return result(-ECHILD);
	int rc = logger_process_ensure();
	if (rc)
		return result(rc);
	int old_cancel;
	rc = lock_scope(&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL,
			&old_cancel);
	if (rc)
		return result(rc);
	rc = operation_lock_nested();
	if (rc) {
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(rc ? rc : control_rc);
	}
	if (g_runtime) {
		int operation_rc = operation_unlock_nested();
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(operation_rc ? operation_rc :
			      control_rc ? control_rc : -EALREADY);
	}
	if (atomic_load_explicit(&g_generation, memory_order_relaxed) ==
	    UINT64_MAX) {
		int operation_rc = operation_unlock_nested();
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(operation_rc ? operation_rc :
			      control_rc ? control_rc : -EOVERFLOW);
	}
	rc = operation_unlock_nested();
	if (rc) {
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(rc ? rc : control_rc);
	}
	atomic_fetch_add_explicit(&g_generation, 1, memory_order_release);
	atomic_store_explicit(&g_phase, AUDIT_STATE_STARTING,
			      memory_order_release);

	audit_runtime_t *candidate = NULL;
	audit_config_t config;
	rc = copy_audit_config(c, &config);
	if (!rc)
		rc = validate_config(&config);
	if (!rc)
		rc = create_runtime(&config, &candidate);
	if (!rc) {
		rc = operation_lock_nested();
		if (rc) {
			atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
					      memory_order_release);
			(void)dispose_runtime(candidate);
			int control_rc = unlock_scope(
				&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL,
				old_cancel);
			return result(rc ? rc : control_rc);
		}
		g_runtime =
			candidate; /* START 与检查点成功后才发布会话。 */
		atomic_store_explicit(&g_policy, config.failure_policy,
				      memory_order_release);
		rc = operation_unlock_nested();
		if (rc) {
			int control_rc = unlock_scope(
				&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL,
				old_cancel);
			return result(rc ? rc : control_rc);
		}
		atomic_store_explicit(&g_phase, AUDIT_STATE_RUNNING,
				      memory_order_release);
	} else {
		audit_status_t failed = { 0 };
		if (candidate)
			snapshot_runtime(candidate, &failed);
		/* 初始化失败不伪造 STOP；清理再失败也保留原始错误。 */
		(void)dispose_runtime(candidate);
		failed.state = AUDIT_STATE_IDLE;
		failed.error_code = -rc;
		int status_rc = operation_lock_nested();
		if (!status_rc) {
			g_last_status = failed;
			status_rc = operation_unlock_nested();
		}
		if (status_rc) {
			g_last_status = failed;
			atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
					      memory_order_release);
		} else {
			atomic_store_explicit(&g_phase, AUDIT_STATE_IDLE,
					      memory_order_release);
		}
	}
	int control_rc = unlock_scope(
		&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
	if (!rc)
		rc = control_rc;
	return result(rc);
}

int audit_shutdown_status(void)
{
	int ready = logger_process_ensure();
	if (ready)
		return result(ready);
	int old_cancel;
	int rc = lock_scope(&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL,
			    &old_cancel);
	if (rc)
		return result(rc);
	atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
			      memory_order_release);
	rc = operation_lock_nested();
	if (rc) {
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(rc ? rc : control_rc);
	}
	audit_runtime_t *s = g_runtime;
	audit_status_t final = g_last_status;
	int operation_rc = operation_unlock_nested();
	if (operation_rc) {
		int control_rc = unlock_scope(
			&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
		return result(operation_rc ? operation_rc : control_rc);
	}
	g_runtime = NULL;
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
			rc = write_runtime(s, &stop, NULL);
		}
		snapshot_runtime(s, &final);
	}
	int close_rc = dispose_runtime(s); /* 此时仍持有 control mutex。 */
	if (!rc)
		rc = close_rc;
	final.state = AUDIT_STATE_IDLE;
	if (rc)
		final.error_code = -rc;
	int status_rc = operation_lock_nested();
	if (!status_rc) {
		g_last_status = final;
		status_rc = operation_unlock_nested();
	}
	if (status_rc) {
		g_last_status = final;
		atomic_store_explicit(&g_phase, AUDIT_STATE_STOPPING,
				      memory_order_release);
		if (!rc)
			rc = status_rc;
	} else {
		atomic_store_explicit(&g_phase, AUDIT_STATE_IDLE,
				      memory_order_release);
	}
	int control_rc = unlock_scope(
		&g_control_mu, LOGGER_LOCK_AUDIT_CONTROL, old_cancel);
	if (!rc)
		rc = control_rc;
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
		rc = write_runtime(s, e, NULL);
		int unlock_rc = unlock_scope(
			&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			old_cancel);
		if (!rc)
			rc = unlock_rc;
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
		/* Allocate tentatively: pure preflight failure must neither change
		 * the caller's event nor consume an automatic transaction ID. */
		int automatic = !next.transaction_id;
		if (automatic)
			next.transaction_id = s->next_txn + 1;
		audit_submission_state_t submission;
		rc = write_runtime(s, &next, &submission);
		if (submission != AUDIT_NOT_SUBMITTED) {
			if (automatic)
				s->next_txn = next.transaction_id;
			/* On an I/O error this is an attempted transaction ID, NOT
			 * proof that the corresponding record was committed. */
			*e = next;
		}
	}
	{
		int unlock_rc = unlock_scope(
			&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			old_cancel);
		if (!rc)
			rc = unlock_rc;
	}
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
		audit_submission_state_t submission;
		rc = write_runtime(s, &next, &submission);
		/* Even when the strict backend failed, the record may be on disk.
		 * Expose the attempted RESULT fields for explicit reconciliation. */
		if (submission != AUDIT_NOT_SUBMITTED)
			*e = next;
	}
	{
		int unlock_rc = unlock_scope(
			&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			old_cancel);
		if (!rc)
			rc = unlock_rc;
	}
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
	{
		int unlock_rc = unlock_scope(
			&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			old_cancel);
		if (!rc)
			rc = unlock_rc;
	}
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
	int rc = lock_scope(&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			    &old_cancel);
	if (rc)
		return result(rc);
	phase = atomic_load_explicit(&g_phase, memory_order_acquire);
	if (phase == AUDIT_STATE_RUNNING && g_runtime)
		snapshot_runtime(g_runtime, out);
	else if (phase == AUDIT_STATE_IDLE)
		*out = g_last_status;
	else
		out->state = (audit_state_t)phase;
	rc = unlock_scope(&g_operation_mu, LOGGER_LOCK_AUDIT_OPERATION,
			  old_cancel);
	return result(rc);
}

audit_failure_policy_t audit_failure_policy(void)
{
	if (logger_process_is_child())
		return AUDIT_FAIL_DENY;

	/*
	 * Snapshot generation before phase. Observing RUNNING publishes that
	 * generation's policy. A second generation load rejects a call that spans
	 * shutdown/reinit, so a previous RUNNING phase can never be paired with a
	 * later session's policy.
	 */
	uint64_t generation =
		atomic_load_explicit(&g_generation, memory_order_acquire);
	int phase = atomic_load_explicit(&g_phase, memory_order_acquire);
	if (phase != AUDIT_STATE_RUNNING)
		return AUDIT_FAIL_DENY;
	audit_failure_policy_t policy =
		(audit_failure_policy_t)atomic_load_explicit(
			&g_policy, memory_order_acquire);
	if (generation != atomic_load_explicit(&g_generation,
					       memory_order_acquire))
		return AUDIT_FAIL_DENY;
	return policy;
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
		char copy[33];
		memcpy(copy, s->instance_id, sizeof(copy));
		rc = unlock_scope(&g_operation_mu,
				  LOGGER_LOCK_AUDIT_OPERATION, old_cancel);
		if (!rc)
			memcpy(out, copy, sizeof(copy));
	}
	return result(rc);
}

const char *audit_instance_id(void)
{
	static _Thread_local char copy[33];
	(void)audit_instance_id_copy(copy);
	return copy;
}
