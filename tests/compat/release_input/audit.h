#ifndef AUDIT_H
#define AUDIT_H
#include "logger.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AUDIT_SUCCESS=0, AUDIT_FAILURE=1 } audit_result_t;
typedef enum { AUDIT_PHASE_ATTEMPT=0, AUDIT_PHASE_RESULT=1 } audit_phase_t;
typedef enum { AUDIT_FAIL_REPORT=0, AUDIT_FAIL_DENY=1 } audit_failure_policy_t;
/* SHA-256 is the only digest algorithm. NONE explicitly disables integrity;
 * it is not an alternative digest. Retired numeric ID 2 is reserved forever
 * and rejected, not remapped. Source callers using the removed algorithm must
 * update their configuration; an existing chain must never be rehashed in place. */
typedef enum { AUDIT_INTEGRITY_NONE=0, AUDIT_INTEGRITY_SHA256=1 } audit_integrity_t;
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
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    const char *log_dir;
    const char *name; /* xxx -> xxx.audit.log */
    const char *chain_state_path; /* NULL -> <log_dir>/<name>.audit.state */
    logger_rotation_config_t rotation;
    audit_failure_policy_t failure_policy;
    audit_integrity_t integrity;
    int fsync_each_record;
} audit_config_t;
#define AUDIT_DEFAULT_CONFIG() ((audit_config_t){ \
 .struct_size=sizeof(audit_config_t),.version=AUDIT_CONFIG_VERSION, \
 .log_dir="/var/log",.name="app", \
 .rotation={.mode=LOGGER_ROTATE_SIZE_DAILY,.max_file_size=100u*1024u*1024u,.retention_days=90}, \
 .failure_policy=AUDIT_FAIL_REPORT,.integrity=AUDIT_INTEGRITY_SHA256,.fsync_each_record=1 })
/* audit_init/audit_write/audit_begin/audit_end/audit_flush use POSIX-style
 * 0 success, -1 failure with errno. Audit is a single-writer process-global
 * subsystem; do not initialize two processes on the same chain destination. */
int audit_init(const audit_config_t *);
/* Always releases the local runtime. Returns -1/errno if STOP, its checkpoint,
 * or an earlier uncertain log/crypto operation failed. The void API is a wrapper. */
int audit_shutdown_status(void);
void audit_shutdown(void);

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
 * session counters. In IDLE the most recent shutdown/init-failure is retained.
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
int audit_get_status(audit_status_t *);
/* Invalidation only. Does NOT reset inherited locks, destroy runtime, or emit
 * STOP. Reinitialization in a forked child returns ECHILD until exec. Prefer
 * fork-before-any-library-runtime-use (single-threaded) or fork+exec. The
 * process guard is shared with Logger/Console; see API.md. */
/* With logger_fork_reinit, Audit must be shut down before entry and may be
 * initialized normally after return. Do not call this invalidation helper on
 * that clean path. */
void audit_after_fork_child(void);
int audit_write(const audit_event_t *);
int audit_begin(audit_event_t *event);
int audit_end(audit_event_t *event, audit_result_t result, int error_code);
/* Repairs a pending checkpoint without duplicating its committed log record.
 * IO_FAILED / CRYPTO_FAILED are not cleared by flush; fix the cause, shut down
 * and reinitialize. IO_FAILED additionally requires reconciliation. */
int audit_flush(void);
audit_failure_policy_t audit_failure_policy(void);
/* Returns a thread-local snapshot (empty on error), replaced by the next call
 * in this thread. Prefer the error-reporting copy API. */
const char *audit_instance_id(void);
int audit_instance_id_copy(char out[33]);
/* Verification returns -1/errno on digest engine failure as well as corrupt
 * input. No zero-digest substitution or backend fallback. See API.md. */
/* Strict current KV record format, final LF required. No repair in verifier.
 * 0 / -1 + errno; EBADMSG for malformed/broken chains, EOVERFLOW for oversized
 * records. The historical presentation prefix is NOT cryptographically covered.
 * *_from() leaves final_hash_hex unchanged on any failure. */
int audit_verify_file(const char *path);
/* Compatibility entry point: only SHA256 is accepted; NONE and unsupported
 * IDs return EPROTONOSUPPORT. audit_verify_file() is the default SHA-256 API. */
int audit_verify_file_with(const char *path, audit_integrity_t algorithm);
/* Compatibility/diagnostic accessor: always returns the static string "builtin". */
const char *audit_crypto_backend(void);
int audit_verify_file_from(const char *path, const char *expected_prev_hex, char final_hash_hex[65]);
#ifdef __cplusplus
}
#endif
#endif
