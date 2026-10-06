#include <audit.h>
#include <console.h>
#include <logger.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define ABI_ASSERT(expr) static_assert((expr), #expr)
#define ABI_ALIGNOF(type) alignof(type)
/* The exported ABI is C. GCC format attributes on declarations are not part of
 * the C function type, but C++ type-trait comparisons warn when those
 * attributes are discarded. Exact function types are therefore frozen by the
 * C11 translation unit below; C++11 independently freezes layouts, numeric
 * constants and default initializers. */
#define ABI_FN(name, ret, ...)
#else
#define ABI_ASSERT(expr) _Static_assert((expr), #expr)
#define ABI_ALIGNOF(type) _Alignof(type)
#define ABI_FN(name, ret, ...) \
	_Static_assert(__builtin_types_compatible_p(__typeof__(&name), \
						   ret (*)(__VA_ARGS__)), \
		       "function type drift: " #name)
#endif

#define ABI_TYPE(type, size_, align_) \
	ABI_ASSERT(sizeof(type) == (size_)); \
	ABI_ASSERT(ABI_ALIGNOF(type) == (align_))

#define ABI_FIELD(type, field, offset_) \
	ABI_ASSERT(offsetof(type, field) == (offset_))

/* Linux/ELF x86_64 Production v1 aggregate layout contract. */
ABI_TYPE(logger_config_t, 144, 8);
ABI_FIELD(logger_config_t, struct_size, 0);
ABI_FIELD(logger_config_t, version, 4);
ABI_FIELD(logger_config_t, level, 8);
ABI_FIELD(logger_config_t, detail, 12);
ABI_FIELD(logger_config_t, outputs, 16);
ABI_FIELD(logger_config_t, file_path, 24);
ABI_FIELD(logger_config_t, rotation, 32);
ABI_FIELD(logger_config_t, file_mode, 56);
ABI_FIELD(logger_config_t, async_mode, 60);
ABI_FIELD(logger_config_t, queue_capacity, 64);
ABI_FIELD(logger_config_t, flush_level, 72);
ABI_FIELD(logger_config_t, include_pid, 76);
ABI_FIELD(logger_config_t, include_tid, 80);
ABI_FIELD(logger_config_t, include_source, 84);
ABI_FIELD(logger_config_t, ident, 88);
ABI_FIELD(logger_config_t, overflow, 96);
ABI_FIELD(logger_config_t, syslog, 120);
ABI_ASSERT(LOGGER_CONFIG_V1_PREFIX_SIZE == 120);

ABI_TYPE(logger_rotation_config_t, 24, 8);
ABI_FIELD(logger_rotation_config_t, mode, 0);
ABI_FIELD(logger_rotation_config_t, max_file_size, 8);
ABI_FIELD(logger_rotation_config_t, retention_days, 16);

ABI_TYPE(logger_source_t, 32, 8);
ABI_FIELD(logger_source_t, module, 0);
ABI_FIELD(logger_source_t, file, 8);
ABI_FIELD(logger_source_t, func, 16);
ABI_FIELD(logger_source_t, line, 24);

ABI_TYPE(logger_context_t, 24, 8);
ABI_FIELD(logger_context_t, request_id, 0);
ABI_FIELD(logger_context_t, session_id, 8);
ABI_FIELD(logger_context_t, trace_id, 16);

ABI_TYPE(logger_metrics_t, 88, 8);
ABI_FIELD(logger_metrics_t, dropped, 0);
ABI_FIELD(logger_metrics_t, queue_high_watermark, 48);
ABI_FIELD(logger_metrics_t, enqueued, 56);
ABI_FIELD(logger_metrics_t, sync_fallbacks, 64);
ABI_FIELD(logger_metrics_t, consumer_batches, 72);
ABI_FIELD(logger_metrics_t, consumer_records, 80);

ABI_TYPE(logger_io_metrics_t, 40, 8);
ABI_FIELD(logger_io_metrics_t, async_completed, 0);
ABI_FIELD(logger_io_metrics_t, sync_completed, 8);
ABI_FIELD(logger_io_metrics_t, emitted_records, 16);
ABI_FIELD(logger_io_metrics_t, failed_records, 24);
ABI_FIELD(logger_io_metrics_t, first_error, 32);

ABI_TYPE(logger_diagnostics_t, 192, 8);
ABI_FIELD(logger_diagnostics_t, state, 0);
ABI_FIELD(logger_diagnostics_t, level, 4);
ABI_FIELD(logger_diagnostics_t, outputs, 8);
ABI_FIELD(logger_diagnostics_t, observation_flags, 12);
ABI_FIELD(logger_diagnostics_t, async_mode, 16);
ABI_FIELD(logger_diagnostics_t, worker_running, 20);
ABI_FIELD(logger_diagnostics_t, consumer_waiting, 24);
ABI_FIELD(logger_diagnostics_t, first_error, 28);
ABI_FIELD(logger_diagnostics_t, dropped_records, 32);
ABI_FIELD(logger_diagnostics_t, sync_fallbacks, 40);
ABI_FIELD(logger_diagnostics_t, enqueued, 48);
ABI_FIELD(logger_diagnostics_t, consumer_batches, 56);
ABI_FIELD(logger_diagnostics_t, consumer_records, 64);
ABI_FIELD(logger_diagnostics_t, async_completed, 72);
ABI_FIELD(logger_diagnostics_t, sync_completed, 80);
ABI_FIELD(logger_diagnostics_t, emitted_records, 88);
ABI_FIELD(logger_diagnostics_t, failed_records, 96);
ABI_FIELD(logger_diagnostics_t, queue_capacity, 104);
ABI_FIELD(logger_diagnostics_t, queue_depth, 112);
ABI_FIELD(logger_diagnostics_t, queue_high_watermark, 120);
ABI_FIELD(logger_diagnostics_t, completion_backlog, 128);
ABI_FIELD(logger_diagnostics_t, queue_storage_bytes, 136);
ABI_FIELD(logger_diagnostics_t, spill_capacity, 144);
ABI_FIELD(logger_diagnostics_t, spill_in_use, 152);
ABI_FIELD(logger_diagnostics_t, spill_exhaustions, 160);
ABI_FIELD(logger_diagnostics_t, worker_wait_count, 168);
ABI_FIELD(logger_diagnostics_t, producer_wake_signals, 176);
ABI_FIELD(logger_diagnostics_t, force_wake_signals, 184);

ABI_TYPE(logger_file_metrics_t, 168, 8);
ABI_FIELD(logger_file_metrics_t, write_operations, 0);
ABI_FIELD(logger_file_metrics_t, failed_write_operations, 8);
ABI_FIELD(logger_file_metrics_t, write_syscalls, 16);
ABI_FIELD(logger_file_metrics_t, interrupted_write_syscalls, 24);
ABI_FIELD(logger_file_metrics_t, failed_write_syscalls, 32);
ABI_FIELD(logger_file_metrics_t, bytes_written, 40);
ABI_FIELD(logger_file_metrics_t, data_sync_attempts, 48);
ABI_FIELD(logger_file_metrics_t, data_sync_failures, 56);
ABI_FIELD(logger_file_metrics_t, directory_sync_attempts, 64);
ABI_FIELD(logger_file_metrics_t, directory_sync_failures, 72);
ABI_FIELD(logger_file_metrics_t, rotation_attempts, 80);
ABI_FIELD(logger_file_metrics_t, rotation_successes, 88);
ABI_FIELD(logger_file_metrics_t, rotation_failures, 96);
ABI_FIELD(logger_file_metrics_t, reopen_attempts, 104);
ABI_FIELD(logger_file_metrics_t, reopen_successes, 112);
ABI_FIELD(logger_file_metrics_t, reopen_failures, 120);
ABI_FIELD(logger_file_metrics_t, retention_deletions, 128);
ABI_FIELD(logger_file_metrics_t, current_size, 136);
ABI_FIELD(logger_file_metrics_t, detached, 144);
ABI_FIELD(logger_file_metrics_t, last_error, 148);
ABI_FIELD(logger_file_metrics_t, last_write_error, 152);
ABI_FIELD(logger_file_metrics_t, last_sync_error, 156);
ABI_FIELD(logger_file_metrics_t, last_rotation_error, 160);
ABI_FIELD(logger_file_metrics_t, last_reopen_error, 164);

ABI_TYPE(logger_syslog_config_t, 24, 8);
ABI_FIELD(logger_syslog_config_t, path, 0);
ABI_FIELD(logger_syslog_config_t, facility, 8);
ABI_FIELD(logger_syslog_config_t, reconnect_interval_ms, 12);
ABI_FIELD(logger_syslog_config_t, startup, 16);

ABI_TYPE(logger_syslog_metrics_t, 104, 8);
ABI_FIELD(logger_syslog_metrics_t, submitted_records, 0);
ABI_FIELD(logger_syslog_metrics_t, sent_records, 8);
ABI_FIELD(logger_syslog_metrics_t, failed_records, 16);
ABI_FIELD(logger_syslog_metrics_t, backpressure_records, 24);
ABI_FIELD(logger_syslog_metrics_t, cooldown_records, 32);
ABI_FIELD(logger_syslog_metrics_t, connect_attempts, 40);
ABI_FIELD(logger_syslog_metrics_t, connect_failures, 48);
ABI_FIELD(logger_syslog_metrics_t, reconnect_successes, 56);
ABI_FIELD(logger_syslog_metrics_t, send_attempts, 64);
ABI_FIELD(logger_syslog_metrics_t, interrupted_calls, 72);
ABI_FIELD(logger_syslog_metrics_t, close_failures, 80);
ABI_FIELD(logger_syslog_metrics_t, connected, 88);
ABI_FIELD(logger_syslog_metrics_t, last_error, 92);
ABI_FIELD(logger_syslog_metrics_t, last_close_error, 96);

ABI_TYPE(audit_config_t, 64, 8);
ABI_ASSERT(AUDIT_CONFIG_V1_SIZE == 64);
ABI_FIELD(audit_config_t, struct_size, 0);
ABI_FIELD(audit_config_t, version, 4);
ABI_FIELD(audit_config_t, log_dir, 8);
ABI_FIELD(audit_config_t, name, 16);
ABI_FIELD(audit_config_t, chain_state_path, 24);
ABI_FIELD(audit_config_t, rotation, 32);
ABI_FIELD(audit_config_t, failure_policy, 56);
ABI_FIELD(audit_config_t, integrity, 60);

ABI_TYPE(audit_event_t, 72, 8);
ABI_FIELD(audit_event_t, phase, 0);
ABI_FIELD(audit_event_t, transaction_id, 8);
ABI_FIELD(audit_event_t, event, 16);
ABI_FIELD(audit_event_t, actor, 24);
ABI_FIELD(audit_event_t, source, 32);
ABI_FIELD(audit_event_t, resource, 40);
ABI_FIELD(audit_event_t, operation, 48);
ABI_FIELD(audit_event_t, result, 56);
ABI_FIELD(audit_event_t, error_code, 60);
ABI_FIELD(audit_event_t, detail, 64);

ABI_TYPE(audit_status_t, 72, 8);
ABI_FIELD(audit_status_t, state, 0);
ABI_FIELD(audit_status_t, error_code, 4);
ABI_FIELD(audit_status_t, checkpoint_dirty, 8);
ABI_FIELD(audit_status_t, committed_seq, 16);
ABI_FIELD(audit_status_t, checkpoint_seq, 24);
ABI_FIELD(audit_status_t, instance_id, 32);

ABI_TYPE(console_config_t, 16, 4);
ABI_FIELD(console_config_t, struct_size, 0);
ABI_FIELD(console_config_t, version, 4);
ABI_FIELD(console_config_t, verbosity, 8);
ABI_FIELD(console_config_t, color, 12);

/* Numeric API contract. */
ABI_ASSERT(LOGGER_TRACE == 0);
ABI_ASSERT(LOGGER_DEBUG == 1);
ABI_ASSERT(LOGGER_INFO == 2);
ABI_ASSERT(LOGGER_WARN == 3);
ABI_ASSERT(LOGGER_ERROR == 4);
ABI_ASSERT(LOGGER_FATAL == 5);
ABI_ASSERT(LOGGER_OFF == 6);
ABI_ASSERT(LOGGER_DETAIL_MINIMAL == 0);
ABI_ASSERT(LOGGER_DETAIL_NORMAL == 1);
ABI_ASSERT(LOGGER_DETAIL_VERBOSE == 2);
ABI_ASSERT(LOGGER_DETAIL_DEBUG == 3);
ABI_ASSERT(LOGGER_OUT_STDERR == 1);
ABI_ASSERT(LOGGER_OUT_FILE == 2);
ABI_ASSERT(LOGGER_OUT_SYSLOG == 4);
ABI_ASSERT(LOGGER_ROTATE_NONE == 0);
ABI_ASSERT(LOGGER_ROTATE_SIZE == 1);
ABI_ASSERT(LOGGER_ROTATE_DAILY == 2);
ABI_ASSERT(LOGGER_ROTATE_SIZE_DAILY == 3);
ABI_ASSERT(LOGGER_ROTATE_EXTERNAL == 4);
ABI_ASSERT(LOGGER_STATE_CREATED == 0);
ABI_ASSERT(LOGGER_STATE_RUNNING == 1);
ABI_ASSERT(LOGGER_STATE_STOPPING == 2);
ABI_ASSERT(LOGGER_STATE_STOPPED == 3);
ABI_ASSERT(LOGGER_OVERFLOW_DROP == 0);
ABI_ASSERT(LOGGER_OVERFLOW_SYNC == 1);
ABI_ASSERT(LOGGER_DIAG_DROPS_OBSERVED == 1);
ABI_ASSERT(LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED == 2);
ABI_ASSERT(LOGGER_DIAG_QUEUE_SATURATED == 4);
ABI_ASSERT(LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED == 8);
ABI_ASSERT(LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED == 16);
ABI_ASSERT(LOGGER_DIAG_WORKER_STOPPED == 32);
ABI_ASSERT(LOGGER_SYSLOG_START_REQUIRED == 0);
ABI_ASSERT(LOGGER_SYSLOG_START_DEFERRED == 1);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_USER == 8);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL0 == 128);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL1 == 136);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL2 == 144);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL3 == 152);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL4 == 160);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL5 == 168);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL6 == 176);
ABI_ASSERT(LOGGER_SYSLOG_FACILITY_LOCAL7 == 184);
ABI_ASSERT(LOGGER_CONFIG_VERSION == 1);

ABI_ASSERT(AUDIT_SUCCESS == 0);
ABI_ASSERT(AUDIT_FAILURE == 1);
ABI_ASSERT(AUDIT_PHASE_ATTEMPT == 0);
ABI_ASSERT(AUDIT_PHASE_RESULT == 1);
ABI_ASSERT(AUDIT_FAIL_REPORT == 0);
ABI_ASSERT(AUDIT_FAIL_DENY == 1);
ABI_ASSERT(AUDIT_INTEGRITY_NONE == 0);
ABI_ASSERT(AUDIT_INTEGRITY_SHA256 == 1);
ABI_ASSERT(AUDIT_STATE_IDLE == 0);
ABI_ASSERT(AUDIT_STATE_STARTING == 1);
ABI_ASSERT(AUDIT_STATE_RUNNING == 2);
ABI_ASSERT(AUDIT_STATE_CHECKPOINT_FAILED == 3);
ABI_ASSERT(AUDIT_STATE_IO_FAILED == 4);
ABI_ASSERT(AUDIT_STATE_STOPPING == 5);
ABI_ASSERT(AUDIT_STATE_FORKED == 6);
ABI_ASSERT(AUDIT_STATE_CRYPTO_FAILED == 7);
ABI_ASSERT(AUDIT_CONFIG_VERSION == 1);

ABI_ASSERT(CONSOLE_COLOR_AUTO == 0);
ABI_ASSERT(CONSOLE_COLOR_ALWAYS == 1);
ABI_ASSERT(CONSOLE_COLOR_NEVER == 2);
ABI_ASSERT(CONSOLE_QUIET == 0);
ABI_ASSERT(CONSOLE_NORMAL == 1);
ABI_ASSERT(CONSOLE_VERBOSE == 2);
ABI_ASSERT(CONSOLE_DEBUG == 3);
ABI_ASSERT(CONSOLE_CONFIG_VERSION == 1);

/* Exact public function type contract: 62 default production exports. */
ABI_FN(audit_begin, int, audit_event_t *);
ABI_FN(audit_crypto_backend, const char *, void);
ABI_FN(audit_end, int, audit_event_t *, audit_result_t, int);
ABI_FN(audit_failure_policy, audit_failure_policy_t, void);
ABI_FN(audit_flush, int, void);
ABI_FN(audit_get_status, int, audit_status_t *);
ABI_FN(audit_init, int, const audit_config_t *);
ABI_FN(audit_instance_id, const char *, void);
ABI_FN(audit_instance_id_copy, int, char *);
ABI_FN(audit_shutdown, void, void);
ABI_FN(audit_shutdown_status, int, void);
ABI_FN(audit_verify_file, int, const char *);
ABI_FN(audit_verify_file_from, int, const char *, const char *, char *);
ABI_FN(audit_verify_file_with, int, const char *, audit_integrity_t);
ABI_FN(audit_write, int, const audit_event_t *);

ABI_FN(console_debug, int, const char *, ...);
ABI_FN(console_debug_source, int, const char *, int, const char *, const char *, ...);
ABI_FN(console_error, int, const char *, ...);
ABI_FN(console_info, int, const char *, ...);
ABI_FN(console_init, void, const console_config_t *);
ABI_FN(console_print, int, const char *, ...);
ABI_FN(console_set_color, void, console_color_mode_t);
ABI_FN(console_set_verbosity, void, console_verbosity_t);
ABI_FN(console_stderr_is_tty, int, void);
ABI_FN(console_stdout_is_tty, int, void);
ABI_FN(console_verbose, int, const char *, ...);
ABI_FN(console_warn, int, const char *, ...);

ABI_FN(logger_context_clear, void, void);
ABI_FN(logger_context_get, logger_context_t, void);
ABI_FN(logger_context_set, void, const logger_context_t *);
ABI_FN(logger_create, logger_t *, const logger_config_t *);
ABI_FN(logger_destroy, void, logger_t *);
ABI_FN(logger_destroy_status, int, logger_t *);
ABI_FN(logger_dropped, uint64_t, const logger_t *);
ABI_FN(logger_flush, void, void);
ABI_FN(logger_flush_instance, void, logger_t *);
ABI_FN(logger_flush_instance_status, int, logger_t *);
ABI_FN(logger_flush_status, int, void);
ABI_FN(logger_get_global_diagnostics, void, logger_diagnostics_t *);
ABI_FN(logger_get_global_file_metrics, int, logger_file_metrics_t *);
ABI_FN(logger_get_global_io_metrics, void, logger_io_metrics_t *);
ABI_FN(logger_get_global_metrics, void, logger_metrics_t *);
ABI_FN(logger_get_diagnostics, void, const logger_t *, logger_diagnostics_t *);
ABI_FN(logger_get_file_metrics, int, logger_t *, logger_file_metrics_t *);
ABI_FN(logger_get_io_metrics, void, const logger_t *, logger_io_metrics_t *);
ABI_FN(logger_get_metrics, void, const logger_t *, logger_metrics_t *);
ABI_FN(logger_get_state, logger_state_t, const logger_t *);
ABI_FN(logger_get_syslog_metrics, int, logger_t *, logger_syslog_metrics_t *);
ABI_FN(logger_global_dropped, uint64_t, void);
ABI_FN(logger_global_write, void, logger_level_t, const char *, const char *, int,
       const char *, const char *, ...);
ABI_FN(logger_init, int, const logger_config_t *);
ABI_FN(logger_log, void, logger_t *, logger_level_t, const char *, const char *, int,
       const char *, const char *, ...);
ABI_FN(logger_log_source, void, logger_t *, logger_level_t, logger_source_t,
       const char *, ...);
ABI_FN(logger_log_sync_status, int, logger_t *, logger_level_t, const char *,
       const char *, int, const char *, const char *, ...);
ABI_FN(logger_mask_secret, int, const char *, char *, size_t, size_t, size_t);
ABI_FN(logger_redact, const char *, void);
ABI_FN(logger_reopen, int, void);
ABI_FN(logger_reopen_instance, int, logger_t *);
ABI_FN(logger_set_instance_level, void, logger_t *, logger_level_t);
ABI_FN(logger_set_level, void, logger_level_t);
ABI_FN(logger_shutdown, void, void);
ABI_FN(logger_shutdown_status, int, void);

static int defaults_match(void)
{
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	if (l.struct_size != 144 || l.version != 1 || l.level != LOGGER_INFO ||
	    l.detail != LOGGER_DETAIL_VERBOSE || l.outputs != LOGGER_OUT_STDERR ||
	    l.file_path != NULL || l.rotation.mode != LOGGER_ROTATE_SIZE_DAILY ||
	    l.rotation.max_file_size != 100u * 1024u * 1024u ||
	    l.rotation.retention_days != 30 || l.file_mode != 0640 ||
	    l.async_mode != 1 || l.queue_capacity != 8192 ||
	    l.flush_level != LOGGER_ERROR || l.include_pid != 1 ||
	    l.include_tid != 1 || l.include_source != 1 || !l.ident ||
	    strcmp(l.ident, "app"))
		return 1;
	const logger_overflow_policy_t overflow[6] = {
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_SYNC, LOGGER_OVERFLOW_SYNC
	};
	for (unsigned i = 0; i < 6; ++i)
		if (l.overflow[i] != overflow[i])
			return 2;
	if (l.syslog.path != NULL ||
	    l.syslog.facility != LOGGER_SYSLOG_FACILITY_USER ||
	    l.syslog.reconnect_interval_ms != 1000u ||
	    l.syslog.startup != LOGGER_SYSLOG_START_REQUIRED)
		return 3;

	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	if (a.struct_size != 64 || a.version != 1 || !a.log_dir ||
	    strcmp(a.log_dir, "/var/log") || !a.name || strcmp(a.name, "app") ||
	    a.chain_state_path != NULL ||
	    a.rotation.mode != LOGGER_ROTATE_SIZE_DAILY ||
	    a.rotation.max_file_size != 100u * 1024u * 1024u ||
	    a.rotation.retention_days != 90 ||
	    a.failure_policy != AUDIT_FAIL_REPORT ||
	    a.integrity != AUDIT_INTEGRITY_SHA256)
		return 4;

	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.struct_size != 16 || c.version != 1 ||
	    c.verbosity != CONSOLE_NORMAL || c.color != CONSOLE_COLOR_AUTO)
		return 5;
	if (strcmp(AUDIT_DETAIL_REDACTED, "[REDACTED]"))
		return 6;
	return 0;
}

int main(void)
{
	return defaults_match();
}
