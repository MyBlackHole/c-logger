#ifndef AUDIT_H
#define AUDIT_H
#include "logger_export.h"
#include "logger.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AUDIT_SUCCESS = 0, AUDIT_FAILURE = 1 } audit_result_t;
typedef enum { AUDIT_PHASE_ATTEMPT = 0, AUDIT_PHASE_RESULT = 1 } audit_phase_t;
typedef enum {
	AUDIT_FAIL_REPORT = 0,
	AUDIT_FAIL_DENY = 1
} audit_failure_policy_t;
/* SHA-256 is the only digest algorithm. NONE explicitly disables integrity;
 * it is not an alternative digest. Retired numeric ID 2 is reserved forever
 * and rejected, not remapped. Source callers using the removed algorithm must
 * update their configuration; an existing chain must never be rehashed in place. */
typedef enum {
	AUDIT_INTEGRITY_NONE = 0,
	AUDIT_INTEGRITY_SHA256 = 1
} audit_integrity_t;
#define AUDIT_DETAIL_REDACTED "[REDACTED]"
/* Audit fields are non-secret identifiers/metadata only. Never place
 * passwords, plaintext keys, private keys, access secrets or bearer/session
 * tokens in actor/source/resource/operation/detail. */
typedef struct {
	audit_phase_t phase;
	uint64_t transaction_id;
	const char *event;
	const char *actor;
	const char *source;
	const char *resource;
	const char *operation;
	audit_result_t result;
	int error_code;
	const char *detail;
} audit_event_t;
#define AUDIT_CONFIG_VERSION 1u
/* Current Production v1 known structure size. Future append-only tails may be
 * passed to an older library only when every unknown byte is zero. */
#define AUDIT_CONFIG_SIZE 64u
/* Audit durability is strict and not configurable: each committed record uses
 * the synchronous forced-sync path before checkpoint advancement.
 * Pre-1.0 callers may advertise a larger struct_size; unknown trailing bytes are
 * ignored, so removal of the historical no-op fsync field remains source/API
 * cleanup without requiring the library to interpret that tail. */
typedef struct {
	uint32_t struct_size; /* must be >= AUDIT_CONFIG_SIZE */

	uint32_t version;
	const char *log_dir;
	const char *name; /* xxx -> xxx.audit.log */
	const char *chain_state_path; /* NULL -> <log_dir>/<name>.audit.state */
	logger_rotation_config_t rotation;
	audit_failure_policy_t failure_policy;
	audit_integrity_t integrity;
} audit_config_t;
#ifdef __cplusplus
static inline audit_config_t audit_defaults_cpp(void)
{
	audit_config_t c = {};
	c.struct_size = sizeof(c);
	c.version = AUDIT_CONFIG_VERSION;
	c.log_dir = "/var/log";
	c.name = "app";
	c.rotation.mode = LOGGER_ROTATE_SIZE_DAILY;
	c.rotation.max_file_size = 100u * 1024u * 1024u;
	c.rotation.retention_days = 90;
	c.failure_policy = AUDIT_FAIL_REPORT;
	c.integrity = AUDIT_INTEGRITY_SHA256;
	return c;
}
#define AUDIT_DEFAULT_CONFIG() audit_defaults_cpp()
#else
#define AUDIT_DEFAULT_CONFIG()                                       \
	((audit_config_t){                                           \
		.struct_size = sizeof(audit_config_t),               \
		.version = AUDIT_CONFIG_VERSION,                     \
		.log_dir = "/var/log",                               \
		.name = "app",                                       \
		.rotation = { .mode = LOGGER_ROTATE_SIZE_DAILY,      \
			      .max_file_size = 100u * 1024u * 1024u, \
			      .retention_days = 90 },                \
		.failure_policy = AUDIT_FAIL_REPORT,                 \
		.integrity = AUDIT_INTEGRITY_SHA256 })
#endif
/* audit_init/audit_write/audit_begin/audit_end/audit_flush use POSIX-style
 * 0 success, -1 failure with errno. Audit is a single-writer process-global
 * subsystem. Writer ownership is the same file-backend lease held on the
 * active Audit log before recovery and retained by the live logger; a second
 * process targeting the same chain returns EBUSY.
 * audit_init also fails with EAGAIN when the kernel CRNG is not ready; it never
 * blocks indefinitely or falls back to weaker randomness for the instance ID. */
LOGGER_API int audit_init(const audit_config_t *);
/* Normally releases the local runtime. A backend I/O/close error may be
 * reported after final free, allowing a later audit_init(). If the private
 * logger's lifetime proof fails (for example, mutex/cond destroy failure),
 * Audit instead retains the entire unpublished runtime and file leases,
 * permanently rejects new sessions and repeated shutdown, and exposes
 * AUDIT_STATE_STOPPING with a sticky error_code until process exit.
 * Returns -1/errno for STOP/checkpoint/older I/O errors as before.
 * The void API is a wrapper. See docs/v2/AUDIT_RETIRE_OWNERSHIP.md. */
LOGGER_API int audit_shutdown_status(void);
LOGGER_API void audit_shutdown(void);

typedef enum {
	AUDIT_STATE_IDLE = 0,
	AUDIT_STATE_STARTING,
	AUDIT_STATE_RUNNING,
	AUDIT_STATE_CHECKPOINT_FAILED,
	AUDIT_STATE_IO_FAILED,
	AUDIT_STATE_STOPPING,
	AUDIT_STATE_FORKED,
	AUDIT_STATE_CRYPTO_FAILED /* appended: previous enum values unchanged */
} audit_state_t;
/* Snapshot only, not a transaction receipt. STARTING/STOPPING/FORKED expose no
 * session counters. STOPPING may expose a sticky error_code for a failed
 * private Logger final-release proof; no recovery is allowed in that process.
 * In IDLE the most recent shutdown/init-failure is retained.
 * committed_seq means confirmed strict-log commit, NOT checkpoint success.
 * An IO_FAILED operation may still have written/persisted bytes.
 * CRYPTO_FAILED precedes output for that record and does not advance its seq. */
typedef struct {
	audit_state_t state;
	int error_code;
	int checkpoint_dirty;
	uint64_t committed_seq;
	uint64_t checkpoint_seq;
	char instance_id[33];
} audit_status_t;
LOGGER_API int audit_get_status(audit_status_t *);
/* Raw-fork inherited Audit state is invalidated internally by the shared
 * process guard. Reinitialization in the child returns ECHILD until exec. */
LOGGER_API int audit_write(const audit_event_t *);
/* 事务事件在纯校验/编码错误（如 EINVAL、EOVERFLOW）时保持原样，
 * 自动生成的 transaction_id 也不消耗。若严格日志后端已被调用，
 * 即使返回 IO_FAILED，event 也携带本次尝试的 transaction_id/phase/
 * result；此时它只是尝试标识，不代表已提交，禁止盲目重试。
 * 若 strict log 已确认持久化而 checkpoint 失败，函数虽然返回 -1，
 * event 仍是已提交的事件快照；audit_get_status() 中 committed_seq
 * 已前进、checkpoint_dirty 为真，audit_flush() 只修复 checkpoint。
 * 不改变公开结构体/函数签名；详见 docs/v2/AUDIT_TRANSACTION_SUBMISSION.md。
 */
LOGGER_API int audit_begin(audit_event_t *event);
LOGGER_API int audit_end(audit_event_t *event, audit_result_t result,
			 int error_code);
/* Repairs a pending checkpoint without duplicating its committed log record.
 * IO_FAILED / CRYPTO_FAILED are not cleared by flush; fix the cause, shut down
 * and reinitialize. IO_FAILED additionally requires reconciliation. */
LOGGER_API int audit_flush(void);
/* Fail closed outside a published RUNNING Audit session. A RUNNING snapshot
 * returns that session's configured policy; STARTING/STOPPING/IDLE/fork child
 * all return AUDIT_FAIL_DENY. */
LOGGER_API audit_failure_policy_t audit_failure_policy(void);
/* Returns a thread-local snapshot (empty on error), replaced by the next call
 * in this thread. Prefer the error-reporting copy API. */
LOGGER_API const char *audit_instance_id(void);
LOGGER_API int audit_instance_id_copy(char out[33]);
/* Verification returns -1/errno on digest engine failure as well as corrupt
 * input. No zero-digest substitution or backend fallback. See API.md. */
/* Strict current KV record format, final LF required. No repair in verifier.
 * 0 / -1 + errno; EBADMSG for malformed/broken chains, EOVERFLOW for oversized
 * records. The historical presentation prefix is NOT cryptographically covered.
 * *_from() leaves final_hash_hex unchanged on any failure. */
LOGGER_API int audit_verify_file(const char *path);
/* Compatibility entry point: only SHA256 is accepted; NONE and unsupported
 * IDs return EPROTONOSUPPORT. audit_verify_file() is the default SHA-256 API. */
LOGGER_API int audit_verify_file_with(const char *path,
				      audit_integrity_t algorithm);
/* Compatibility/diagnostic accessor: always returns the static string "builtin". */
LOGGER_API const char *audit_crypto_backend(void);
LOGGER_API int audit_verify_file_from(const char *path,
				      const char *expected_prev_hex,
				      char final_hash_hex[65]);
#ifdef __cplusplus
}
#endif
#endif
