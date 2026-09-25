#ifndef LOGGER_H
#define LOGGER_H
#include "logger_export.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
	LOGGER_TRACE,
	LOGGER_DEBUG,
	LOGGER_INFO,
	LOGGER_WARN,
	LOGGER_ERROR,
	LOGGER_FATAL,
	LOGGER_OFF
} logger_level_t;
typedef enum {
	LOGGER_DETAIL_MINIMAL = 0,
	LOGGER_DETAIL_NORMAL,
	LOGGER_DETAIL_VERBOSE,
	LOGGER_DETAIL_DEBUG
} logger_detail_t;
typedef struct {
	const char *request_id;
	const char *session_id;
	const char *trace_id;
} logger_context_t;
typedef enum {
	LOGGER_OUT_STDERR = 1u << 0,
	LOGGER_OUT_FILE = 1u << 1,
	LOGGER_OUT_SYSLOG = 1u << 2
} logger_output_t;
typedef enum {
	LOGGER_ROTATE_NONE = 0,
	LOGGER_ROTATE_SIZE,
	LOGGER_ROTATE_DAILY,
	LOGGER_ROTATE_SIZE_DAILY,
	LOGGER_ROTATE_EXTERNAL
} logger_rotate_mode_t;
typedef struct {
	logger_rotate_mode_t mode;
	size_t max_file_size;
	unsigned retention_days;
} logger_rotation_config_t;
typedef struct logger logger_t;
typedef enum {
	LOGGER_STATE_CREATED = 0,
	LOGGER_STATE_RUNNING,
	LOGGER_STATE_STOPPING,
	LOGGER_STATE_STOPPED
} logger_state_t;
typedef struct {
	const char *module;
	const char *file;
	const char *func;
	int line;
} logger_source_t;
#ifdef __cplusplus
#define LOGGER_SOURCE() \
	(logger_source_t{ LOG_MODULE, __FILE__, __func__, __LINE__ })
#else
#define LOGGER_SOURCE()                           \
	((logger_source_t){ .module = LOG_MODULE, \
			    .file = __FILE__,     \
			    .func = __func__,     \
			    .line = __LINE__ })
#endif

typedef enum {
	LOGGER_OVERFLOW_DROP = 0,
	LOGGER_OVERFLOW_SYNC = 1
} logger_overflow_policy_t;
typedef struct {
	uint64_t dropped[6]; /* queue-full DROP only; excludes sync fallback */
	uint64_t queue_high_watermark;
	uint64_t enqueued;
	uint64_t sync_fallbacks;
	uint64_t consumer_batches;
	uint64_t consumer_records; /* dequeued records, not output completion */
} logger_metrics_t;
/* Separate extension: logger_metrics_t retains its original binary layout.
 * Live snapshots may span concurrent updates. After flush with producers
 * quiesced, completed == emitted + failed. Failed means not confirmed across
 * every selected output (a partial batch may contain already-written bytes). */
typedef struct {
	uint64_t async_completed;
	uint64_t sync_completed;
	uint64_t emitted_records;
	uint64_t failed_records;
	int first_error; /* positive errno; never cleared within an instance */
} logger_io_metrics_t;
/* Local Unix datagram Syslog. No per-sink spool or blocking retry queue.
 * REQUIRED preserves create-time connection failure; DEFERRED permits only
 * transient endpoint unavailability and exposes it in metrics/sticky status.
 */
typedef enum {
	LOGGER_SYSLOG_START_REQUIRED = 0,
	LOGGER_SYSLOG_START_DEFERRED = 1
} logger_syslog_start_t;
/* Namespaced facility values avoid requiring <syslog.h>, whose LOG_* macros
 * conflict with the legacy global convenience macros. Other standard facility
 * numbers 0..23 are accepted as (number * 8). */
#define LOGGER_SYSLOG_FACILITY_USER (1u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL0 (16u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL1 (17u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL2 (18u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL3 (19u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL4 (20u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL5 (21u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL6 (22u << 3)
#define LOGGER_SYSLOG_FACILITY_LOCAL7 (23u << 3)
typedef struct {
	const char *path; /* NULL: /dev/log; copied at create */
	unsigned facility; /* namespaced facility or standard number * 8 */
	uint32_t reconnect_interval_ms; /* 0: 1000 ms; otherwise 1..60000 */
	logger_syslog_start_t startup;
} logger_syslog_config_t;
#ifdef __cplusplus
static inline logger_syslog_config_t logger_syslog_defaults_cpp(void)
{
	logger_syslog_config_t c = {};
	c.facility = LOGGER_SYSLOG_FACILITY_USER;
	c.reconnect_interval_ms = 1000u;
	c.startup = LOGGER_SYSLOG_START_REQUIRED;
	return c;
}
#define LOGGER_SYSLOG_DEFAULT_CONFIG() logger_syslog_defaults_cpp()
#else
#define LOGGER_SYSLOG_DEFAULT_CONFIG()                                      \
	((logger_syslog_config_t){ .path = NULL,                            \
				   .facility = LOGGER_SYSLOG_FACILITY_USER, \
				   .reconnect_interval_ms = 1000u,          \
				   .startup = LOGGER_SYSLOG_START_REQUIRED })
#endif

/* Snapshot taken under the output mutex. connected is a local association,
 * NOT a peer health check. send success is NOT durable delivery acknowledgement.
 * failed_records includes backpressure, cooldown rejection and format errors;
 * backpressure_records/cooldown_records are subsets. Initial deferred failures
 * change last_error and the instance's sticky first_error, not record counts.
 */
typedef struct {
	uint64_t submitted_records;
	uint64_t sent_records;
	uint64_t failed_records;
	uint64_t backpressure_records;
	uint64_t cooldown_records;
	uint64_t connect_attempts; /* one socket+connect cycle, including initial */
	uint64_t connect_failures;
	uint64_t reconnect_successes; /* successful cycles after the initial cycle */
	uint64_t send_attempts; /* syscall count, including bounded EINTR retry */
	uint64_t interrupted_calls;
	uint64_t close_failures;
	int connected;
	int last_error; /* latest operation; cleared by a successful send */
	int last_close_error; /* latest cleanup failure; not cleared by send */
} logger_syslog_metrics_t;

#define LOGGER_CONFIG_VERSION 1u
typedef struct {
	uint32_t struct_size;
	uint32_t version;
	logger_level_t level;
	logger_detail_t detail;
	unsigned outputs;
	const char *file_path;
	logger_rotation_config_t rotation;
	unsigned file_mode;
	int async_mode;
	size_t queue_capacity;
	logger_level_t flush_level;
	int include_pid, include_tid, include_source;
	const char *ident;
	logger_overflow_policy_t overflow[6];
	/* Append-only extension. Older version-1 prefix sizes use Syslog defaults.
     * Supply the complete extension or no extension; partial tails are rejected.
     * Pre-versioned binaries are not supported. See docs/SYSLOG_BACKEND.md. */
	logger_syslog_config_t syslog;
} logger_config_t;
#define LOGGER_CONFIG_V1_PREFIX_SIZE offsetof(logger_config_t, syslog)
/* Ownership/threading:
 * - logger_create() returns an owned instance; logger_destroy() releases it.
 * - cfg is borrowed only during create; do not mutate it concurrently.
 * - a business .so borrows its host's instance; only the owner destroys it.
 * - each async instance has its own worker and queue. Merely loading the .so
 *   starts no worker, opens no log and does not initialize the global facade.
 * - logging/level/metrics/flush may be called concurrently while the instance
 *   is alive.
 * - the owner must stop/join all explicit-instance users before destroy.
 * Errors: pointer-returning creation uses NULL + errno; int public operations
 * use 0 on success and -1 with errno unless explicitly documented otherwise.
 */
/* Regular files are single-owner targets (<basename>.logger.lock). A second
 * independent owner returns EBUSY: share one live instance instead. Directory
 * resolution is bound at creation; final symlinks/hardlinks are rejected.
 * Internal rotation requires non-overwriting rename support. See
 * docs/FILE_BACKEND.md for compatibility, resource costs and platform limits. */
/* Explicit calls protect library-owned resources from thread cancellation until
 * their critical work/va_end/unlocks have finished. No rollback/I/O deadline is
 * implied. Same-thread nested resource operations (including another instance,
 * Console and callbacks from an async worker) are rejected with EDEADLK before
 * locks/side effects; status-returning sync_log keeps its -errno convention.
 * This is not signal-safety or general asynchronous-cancellation safety.
 *
 * create has a deferred-cancellation checkpoint before handing off the pointer:
 * a pending request disposes any unreturned candidate. With cancellation ENABLED
 * and type ASYNCHRONOUS it fails ENOTSUP before allocating instance/backend
 * resources (process-guard registration may occur); disable
 * cancellation across pointer ownership transfer instead. The caller owns any
 * successfully returned pointer and must arrange cleanup for later cancellation.
 */
LOGGER_API logger_t *logger_create(const logger_config_t *);
LOGGER_API void logger_context_set(const logger_context_t *);
LOGGER_API void logger_context_clear(void);
LOGGER_API logger_context_t logger_context_get(void);

/* Explicit secret-handling helpers. The logger does not inspect arbitrary
 * printf text for secrets. Callers must redact before logging sensitive data. */
LOGGER_API const char *logger_redact(void);
LOGGER_API int logger_mask_secret(const char *value, char *out, size_t out_size,
				  size_t visible_prefix, size_t visible_suffix);

LOGGER_API void logger_log_source(logger_t *, logger_level_t, logger_source_t,
				  const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 4, 5)))
#endif
	;

/* Exclusive owner only: first stop/join every user. Returns 0 or -1+errno.
 * On an ordinary I/O failure the instance IS freed; never retry the pointer.
 * NULL succeeds. ECHILD refuses inherited state without touching/freeing it.
 * Cancellation is deferred until teardown is complete. No implicit refcount. */
LOGGER_API int logger_destroy_status(logger_t *);
/* Compatibility wrapper, discards teardown status. */
/* A pending cancellation at the end of dispose may run host cleanup after the
 * pointer was freed. Remove it from host cleanup ownership BEFORE invoking
 * destroy at a valid exclusive-owner boundary, or disable cancellation across
 * pointer clearing + disposal. Nested/child rejection occurs before disposal.
 * Never retry a freed pointer just because final I/O returned an error. */
LOGGER_API void logger_destroy(logger_t *);
LOGGER_API logger_state_t logger_get_state(const logger_t *);
/* Input strings must be valid for the duration of the call. Async records
 * snapshot only metadata required by detail/include_*; when module/source are
 * required they are copied as module (127 bytes), source basename (255) and
 * function (127). DEBUG context is copied only when that detail can emit it.
 * Longer source labels are visibly shortened with '~'; no queued pointer needed
 * by later formatting retains storage from an unloadable business .so.
 * This does NOT permit unloading liblogger itself or the callback provider
 * before outstanding calls and logger workers have finished. */
LOGGER_API void logger_log(logger_t *, logger_level_t, const char *,
			   const char *, int, const char *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* Advanced synchronous status path. Returns 0 or negative errno internally;
 * retained for compatibility; new application code should normally use
 * logger_log()/LOG_* and logger_flush_instance(). */
LOGGER_API int logger_log_sync_status(logger_t *, logger_level_t, const char *,
				      const char *, int, const char *,
				      const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 7, 8)))
#endif
	;
/* Status flush waits for the queue reservation watermark captured at entry,
 * including batches already dequeued but not yet emitted, then fsyncs the file
 * backend. Concurrent calls may also be included; queue-full drops are NOT
 * accepted records. Successful sync logging calls preceding flush are covered.
 * Returns 0, or -1 + errno. Earlier output/sync failures remain sticky.
 * This is not a remote syslog persistence acknowledgement or an I/O timeout.
 * Explicit-instance lifetime must be protected by the caller throughout. */
LOGGER_API int logger_flush_instance_status(logger_t *);
/* Compatibility wrapper: same wait, discards status; NULL is a no-op. */
LOGGER_API void logger_flush_instance(logger_t *);
LOGGER_API int logger_reopen_instance(logger_t *);
LOGGER_API void logger_set_instance_level(logger_t *, logger_level_t);
LOGGER_API uint64_t logger_dropped(const logger_t *);
LOGGER_API void logger_get_metrics(const logger_t *, logger_metrics_t *);
LOGGER_API void logger_get_io_metrics(const logger_t *, logger_io_metrics_t *);
/* Live instance required; same external lifetime protection as other controls.
 * 0 / -1+errno. ENOTSUP if Syslog is not selected; output unchanged on error.
 * This does not connect, retry, flush, or reset sticky I/O errors. */
LOGGER_API int logger_get_syslog_metrics(logger_t *, logger_syslog_metrics_t *);
/* Optional application-global facade. Concurrent data/control calls pin one
 * generation and never silently operate on a newer instance. During startup:
 * EAGAIN; during/after shutdown or on a stale pin: ESHUTDOWN; no initial
 * instance: ENODEV (LOG_* alone retains the legacy bootstrap stderr fallback).
 * Nested global calls on the same thread fail EDEADLK. Not signal-safe.
 * Global calls defer cancellation until all library locks/pins are released;
 * cancellation does not roll back output or a completed init/shutdown, nor
 * impose an I/O timeout. Explicit-instance operations use the inner cancellation/reentry scope above.
 */
LOGGER_API int logger_init(const logger_config_t *);
/* Serializes admission closure, drain, join, close and owner release. Same
 * generation repeated shutdown succeeds; a delayed shutdown never destroys a
 * newer generation (ESHUTDOWN). On I/O failure the old instance is still freed;
 * the error is returned once to the controller, not a retryable pointer.
 * Unlike void wrapper, permits checking the final synchronization/close result.
 */
LOGGER_API int logger_shutdown_status(void);
LOGGER_API void logger_shutdown(void);
/* same guarantee for the global facade */
LOGGER_API int logger_flush_status(void);
LOGGER_API void logger_flush(void); /* compatibility wrapper; no error return */
LOGGER_API int logger_reopen(void);
/* Raw fork contract (library does not manage the host process):
 * - fork before any library runtime use while truly single-threaded, OR
 *   fork+exec. After async initialization the process is already multithreaded.
 * - raw-fork inherited Logger/Console/Audit runtimes require exec to reinitialize.
 *   Constructors/status APIs reject with ECHILD (sync_status returns -ECHILD);
 *   void logging/destruction/controls no-op and set errno; metrics return zero,
 *   get_state returns STOPPED with errno=ECHILD, not a disposal acknowledgement.
 * - prepare/parent are compatibility markers only: no lock held across fork,
 *   no queue flush/freeze. Child helper only invalidates, never resets locks or
 *   frees/closes inherited state. Calling it does NOT enable reinitialization.
 * - do not use these helpers with vfork, in signal handlers or to make arbitrary
 *   post-fork calls safe. Arguments to LOG_* are evaluated before rejection.
 * Parent descriptors/queues remain unchanged; child fds live until exec/_exit.
 */
/* Optional legacy application helper is declared only by logger_fork_compat.h.
 * The normal library never calls fork or scans the host's thread list. */
#if defined(LOGGER_ENABLE_LEGACY_FORK_HELPER) && \
	LOGGER_ENABLE_LEGACY_FORK_HELPER
#include "logger_fork_compat.h"
#endif

LOGGER_API int logger_prepare_fork(void);
LOGGER_API void logger_after_fork_parent(void);
LOGGER_API void logger_after_fork_child(void);
LOGGER_API void logger_set_level(logger_level_t);
LOGGER_API uint64_t logger_global_dropped(void);
LOGGER_API void logger_get_global_metrics(logger_metrics_t *);
LOGGER_API void logger_get_global_io_metrics(logger_io_metrics_t *);
LOGGER_API void logger_global_write(logger_level_t, const char *, const char *,
				    int, const char *, const char *, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 6, 7)))
#endif
	;
#ifndef LOG_MODULE
#define LOG_MODULE "app"
#endif
#define LOG_TRACE(...)                                                    \
	logger_global_write(LOGGER_TRACE, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_DEBUG(...)                                                    \
	logger_global_write(LOGGER_DEBUG, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_INFO(...)                                                    \
	logger_global_write(LOGGER_INFO, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_WARN(...)                                                    \
	logger_global_write(LOGGER_WARN, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_ERROR(...)                                                    \
	logger_global_write(LOGGER_ERROR, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOG_FATAL(...)                                                    \
	logger_global_write(LOGGER_FATAL, LOG_MODULE, __FILE__, __LINE__, \
			    __func__, __VA_ARGS__)
#define LOGGER_LOG(instance, level, module, ...)                      \
	logger_log((instance), (level), (module), __FILE__, __LINE__, \
		   __func__, __VA_ARGS__)
#define LOGGER_TRACE(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_TRACE, (module), __VA_ARGS__)
#define LOGGER_DEBUG(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_DEBUG, (module), __VA_ARGS__)
#define LOGGER_INFO(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_INFO, (module), __VA_ARGS__)
#define LOGGER_WARN(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_WARN, (module), __VA_ARGS__)
#define LOGGER_ERROR(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_ERROR, (module), __VA_ARGS__)
#define LOGGER_FATAL(instance, module, ...) \
	LOGGER_LOG((instance), LOGGER_FATAL, (module), __VA_ARGS__)
#ifdef __cplusplus
static inline logger_config_t logger_defaults_cpp(void)
{
	logger_config_t c = {};
	c.struct_size = sizeof(c);
	c.version = LOGGER_CONFIG_VERSION;
	c.level = LOGGER_INFO;
	c.detail = LOGGER_DETAIL_VERBOSE;
	c.outputs = LOGGER_OUT_STDERR;
	c.rotation.mode = LOGGER_ROTATE_SIZE_DAILY;
	c.rotation.max_file_size = 100u * 1024u * 1024u;
	c.rotation.retention_days = 30;
	c.file_mode = 0640;
	c.async_mode = 1;
	c.queue_capacity = 8192;
	c.flush_level = LOGGER_ERROR;
	c.include_pid = c.include_tid = c.include_source = 1;
	c.ident = "app";
	c.overflow[LOGGER_ERROR] = c.overflow[LOGGER_FATAL] =
		LOGGER_OVERFLOW_SYNC;
	c.syslog = LOGGER_SYSLOG_DEFAULT_CONFIG();
	return c;
}
#define LOGGER_DEFAULT_CONFIG() logger_defaults_cpp()
#else
#define LOGGER_DEFAULT_CONFIG()                                             \
	((logger_config_t){                                                 \
		.struct_size = sizeof(logger_config_t),                     \
		.version = LOGGER_CONFIG_VERSION,                           \
		.level = LOGGER_INFO,                                       \
		.detail = LOGGER_DETAIL_VERBOSE,                            \
		.outputs = LOGGER_OUT_STDERR,                               \
		.file_path = NULL,                                          \
		.rotation = { .mode = LOGGER_ROTATE_SIZE_DAILY,             \
			      .max_file_size = 100u * 1024u * 1024u,        \
			      .retention_days = 30 },                       \
		.file_mode = 0640,                                          \
		.async_mode = 1,                                            \
		.queue_capacity = 8192,                                     \
		.flush_level = LOGGER_ERROR,                                \
		.include_pid = 1,                                           \
		.include_tid = 1,                                           \
		.include_source = 1,                                        \
		.ident = "app",                                             \
		.overflow = { LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,   \
			      LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,   \
			      LOGGER_OVERFLOW_SYNC, LOGGER_OVERFLOW_SYNC }, \
		.syslog = LOGGER_SYSLOG_DEFAULT_CONFIG() })
#endif
#ifdef __cplusplus
}
#endif
#endif
