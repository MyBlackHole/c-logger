#include <console.h>
#include <logger.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define ABI_ASSERT(expr) static_assert((expr), #expr)
#define ABI_ALIGNOF(type) alignof(type)
/* 导出 ABI 为 C。声明上的 GCC 格式属性不是 C 函数类型的一部分，
 * 但 C++ 类型特征比较会在这些属性被丢弃时发出警告。
 * 因此 ABI_FN 的精确类型断言由 C11 编译单元冻结；C++11 独立冻结
 * 布局、数值常量与默认初始化器，并通过下方 16 个记录与查询入口的
 * 真实函数指针赋值和调用验证新函数类型及行为。
 */
#define ABI_FN(name, ret, ...) \
	static_assert(true, "C function types are checked by the C11 contract")
#else
#define ABI_ASSERT(expr) _Static_assert((expr), #expr)
#define ABI_ALIGNOF(type) _Alignof(type)
#define ABI_FN(name, ret, ...)                                             \
	_Static_assert(__builtin_types_compatible_p(__typeof__(&name),     \
						    ret (*)(__VA_ARGS__)), \
		       "function type drift: " #name)
#endif

#define ABI_TYPE(type, size_, align_)        \
	ABI_ASSERT(sizeof(type) == (size_)); \
	ABI_ASSERT(ABI_ALIGNOF(type) == (align_))

#define ABI_FIELD(type, field, offset_) \
	ABI_ASSERT(offsetof(type, field) == (offset_))

/* Linux/ELF x86_64 当前 ABI 2 聚合类型布局契约。 */
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
ABI_ASSERT(LOGGER_CONFIG_SIZE == 144);

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

ABI_TYPE(console_config_t, 16, 4);
ABI_ASSERT(CONSOLE_CONFIG_SIZE == 16);
ABI_FIELD(console_config_t, struct_size, 0);
ABI_FIELD(console_config_t, version, 4);
ABI_FIELD(console_config_t, verbosity, 8);
ABI_FIELD(console_config_t, color, 12);

/* 数值 API 契约。 */
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

ABI_ASSERT(CONSOLE_COLOR_AUTO == 0);
ABI_ASSERT(CONSOLE_COLOR_ALWAYS == 1);
ABI_ASSERT(CONSOLE_COLOR_NEVER == 2);
ABI_ASSERT(CONSOLE_QUIET == 0);
ABI_ASSERT(CONSOLE_NORMAL == 1);
ABI_ASSERT(CONSOLE_VERBOSE == 2);
ABI_ASSERT(CONSOLE_DEBUG == 3);
ABI_ASSERT(CONSOLE_CONFIG_VERSION == 1);

/* 精确公开函数类型契约：43 个当前生产导出。 */

ABI_FN(console_debug, int, const char *, ...);
ABI_FN(console_debug_source, int, const char *, int, const char *, const char *,
       ...);
ABI_FN(console_error, int, const char *, ...);
ABI_FN(console_info, int, const char *, ...);
ABI_FN(console_init, int, const console_config_t *);
ABI_FN(console_print, int, const char *, ...);
ABI_FN(console_set_color, int, console_color_mode_t);
ABI_FN(console_set_verbosity, int, console_verbosity_t);
ABI_FN(console_stderr_is_tty, int, int *);
ABI_FN(console_stdout_is_tty, int, int *);
ABI_FN(console_verbose, int, const char *, ...);
ABI_FN(console_warn, int, const char *, ...);

ABI_FN(logger_context_clear, int, void);
ABI_FN(logger_context_get, int, logger_context_t *);
ABI_FN(logger_context_set, int, const logger_context_t *);
ABI_FN(logger_process_init, int, void);
ABI_FN(logger_create, int, const logger_config_t *, logger_t **);
ABI_FN(logger_destroy, int, logger_t **);
ABI_FN(logger_dropped, int, const logger_t *, uint64_t *);
ABI_FN(logger_flush, int, logger_t *);
ABI_FN(logger_default_flush, int, void);
ABI_FN(logger_get_global_diagnostics, int, logger_diagnostics_t *);
ABI_FN(logger_get_global_file_metrics, int, logger_file_metrics_t *);
ABI_FN(logger_get_global_io_metrics, int, logger_io_metrics_t *);
ABI_FN(logger_get_global_metrics, int, logger_metrics_t *);
ABI_FN(logger_get_diagnostics, int, const logger_t *, logger_diagnostics_t *);
ABI_FN(logger_get_file_metrics, int, logger_t *, logger_file_metrics_t *);
ABI_FN(logger_get_io_metrics, int, const logger_t *, logger_io_metrics_t *);
ABI_FN(logger_get_metrics, int, const logger_t *, logger_metrics_t *);
ABI_FN(logger_get_state, int, const logger_t *, logger_state_t *);
ABI_FN(logger_get_syslog_metrics, int, logger_t *, logger_syslog_metrics_t *);
ABI_FN(logger_global_dropped, int, uint64_t *);
ABI_FN(logger_global_write, int, logger_level_t, const char *, const char *,
       int, const char *, const char *, ...);
ABI_FN(logger_default_set, int, logger_t *, logger_t *);
ABI_FN(logger_log, int, logger_t *, logger_level_t, const char *, const char *,
       int, const char *, const char *, ...);
ABI_FN(logger_log_source, int, logger_t *, logger_level_t, logger_source_t,
       const char *, ...);
ABI_FN(logger_log_sync_status, int, logger_t *, logger_level_t, const char *,
       const char *, int, const char *, const char *, ...);
ABI_FN(logger_mask_secret, int, const char *, char *, size_t, size_t, size_t);
ABI_FN(logger_redact, const char *, void);
ABI_FN(logger_default_reopen, int, void);
ABI_FN(logger_reopen, int, logger_t *);
ABI_FN(logger_set_instance_level, int, logger_t *, logger_level_t);
ABI_FN(logger_set_level, int, logger_level_t);

static int query_sentinel(const void *out, size_t size)
{
	const unsigned char *bytes = (const unsigned char *)out;
	for (size_t i = 0; i < size; ++i)
		if (bytes[i] != 0xa5)
			return 0;
	return 1;
}

/* C 与 C++11 都实际调用全部记录/查询类型，不以导出名称代替函数类型。 */
static int record_query_contract(logger_t *owner, int cold)
{
	int (*record)(logger_t *, logger_level_t, const char *, const char *,
		      int, const char *, const char *, ...) = logger_log;
	int (*source_record)(logger_t *, logger_level_t, logger_source_t,
			     const char *, ...) = logger_log_source;
	int (*global_record)(logger_level_t, const char *, const char *, int,
			     const char *, const char *,
			     ...) = logger_global_write;
	int (*strict_record)(logger_t *, logger_level_t, const char *,
			     const char *, int, const char *, const char *,
			     ...) = logger_log_sync_status;
	int (*state_query)(const logger_t *, logger_state_t *) =
		logger_get_state;
	int (*dropped_query)(const logger_t *, uint64_t *) = logger_dropped;
	int (*global_dropped_query)(uint64_t *) = logger_global_dropped;
	int (*metrics_query)(const logger_t *, logger_metrics_t *) =
		logger_get_metrics;
	int (*io_query)(const logger_t *, logger_io_metrics_t *) =
		logger_get_io_metrics;
	int (*diagnostics_query)(const logger_t *, logger_diagnostics_t *) =
		logger_get_diagnostics;
	int (*global_metrics_query)(logger_metrics_t *) =
		logger_get_global_metrics;
	int (*global_io_query)(logger_io_metrics_t *) =
		logger_get_global_io_metrics;
	int (*global_diagnostics_query)(logger_diagnostics_t *) =
		logger_get_global_diagnostics;
	int (*file_query)(logger_t *, logger_file_metrics_t *) =
		logger_get_file_metrics;
	int (*global_file_query)(logger_file_metrics_t *) =
		logger_get_global_file_metrics;
	int (*syslog_query)(logger_t *, logger_syslog_metrics_t *) =
		logger_get_syslog_metrics;
	logger_source_t source = LOGGER_SOURCE();
	int expected = cold ? -ENODEV : 0;
	int backend_error = cold ? -ENODEV : -ENOTSUP;
	logger_state_t state = LOGGER_STATE_CREATED;
	uint64_t dropped = UINT64_C(777);
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_diagnostics_t diagnostics;
	logger_file_metrics_t file;
	logger_syslog_metrics_t syslog;

	errno = EDOM;
	if (record(owner, LOGGER_INFO, "abi", NULL, 0, NULL, "record") !=
		    expected ||
	    errno != EDOM ||
	    source_record(owner, LOGGER_INFO, source, "source") != expected ||
	    errno != EDOM ||
	    global_record(LOGGER_INFO, "abi", NULL, 0, NULL, "default") !=
		    expected ||
	    errno != EDOM ||
	    strict_record(owner, LOGGER_INFO, "abi", NULL, 0, NULL, "strict") !=
		    expected ||
	    errno != EDOM)
		return 1;
	if (state_query(owner, &state) != expected || errno != EDOM ||
	    state != (cold ? LOGGER_STATE_CREATED : LOGGER_STATE_RUNNING))
		return 2;
	if (dropped_query(owner, &dropped) != expected || errno != EDOM ||
	    dropped != (cold ? UINT64_C(777) : 0) ||
	    global_dropped_query(&dropped) != expected || errno != EDOM ||
	    dropped != (cold ? UINT64_C(777) : 0))
		return 3;
	memset(&metrics, 0xa5, sizeof(metrics));
	if (metrics_query(owner, &metrics) != expected || errno != EDOM ||
	    (cold ? !query_sentinel(&metrics, sizeof(metrics)) :
		    metrics.enqueued != 0 || metrics.sync_fallbacks != 0) ||
	    global_metrics_query(&metrics) != expected || errno != EDOM ||
	    (cold ? !query_sentinel(&metrics, sizeof(metrics)) :
		    metrics.enqueued != 0 || metrics.sync_fallbacks != 0))
		return 4;
	memset(&io, 0xa5, sizeof(io));
	if (io_query(owner, &io) != expected || errno != EDOM ||
	    (cold ? !query_sentinel(&io, sizeof(io)) :
		    io.emitted_records != 4 || io.failed_records != 0) ||
	    global_io_query(&io) != expected || errno != EDOM ||
	    (cold ? !query_sentinel(&io, sizeof(io)) :
		    io.emitted_records != 4 || io.failed_records != 0))
		return 5;
	memset(&diagnostics, 0xa5, sizeof(diagnostics));
	if (diagnostics_query(owner, &diagnostics) != expected ||
	    errno != EDOM ||
	    (cold ? !query_sentinel(&diagnostics, sizeof(diagnostics)) :
		    diagnostics.state != LOGGER_STATE_RUNNING) ||
	    global_diagnostics_query(&diagnostics) != expected ||
	    errno != EDOM ||
	    (cold ? !query_sentinel(&diagnostics, sizeof(diagnostics)) :
		    diagnostics.state != LOGGER_STATE_RUNNING))
		return 6;
	memset(&file, 0xa5, sizeof(file));
	if (file_query(owner, &file) != backend_error || errno != EDOM ||
	    !query_sentinel(&file, sizeof(file)) ||
	    global_file_query(&file) != backend_error || errno != EDOM ||
	    !query_sentinel(&file, sizeof(file)))
		return 7;
	memset(&syslog, 0xa5, sizeof(syslog));
	if (syslog_query(owner, &syslog) != backend_error || errno != EDOM ||
	    !query_sentinel(&syslog, sizeof(syslog)))
		return 8;
	return 0;
}

/* 安装包 C/C++11 消费者不仅检查函数指针类型，还实际执行 TLS 合同。 */
static int context_contract(int cold)
{
	int (*set)(const logger_context_t *) = logger_context_set;
	int (*clear)(void) = logger_context_clear;
	int (*get)(logger_context_t *) = logger_context_get;
	logger_context_t input = { "installed-context", "installed-session",
				   NULL };
	logger_context_t output = { "unchanged", "unchanged", "unchanged" };
	logger_context_t before = output;
	errno = EDOM;
	if (cold) {
		if (set(&input) != -ENODEV || errno != EDOM ||
		    get(&output) != -ENODEV || errno != EDOM ||
		    memcmp(&output, &before, sizeof(output)) != 0 ||
		    clear() != -ENODEV || errno != EDOM)
			return 1;
		return 0;
	}
	if (set(&input) || errno != EDOM || get(&output) || errno != EDOM)
		return 2;
	if (!output.request_id || !output.session_id || output.trace_id)
		return 2;
	if (strcmp(output.request_id, "installed-context"))
		return 2;
	if (strcmp(output.session_id, "installed-session"))
		return 2;
	if (get(NULL) != -EINVAL || errno != EDOM)
		return 3;
	if (clear() || errno != EDOM || get(&output) || errno != EDOM)
		return 3;
	if (output.request_id || output.session_id || output.trace_id)
		return 3;
	return 0;
}

/* 安装消费同时执行类型及真实行为，不以旧 ABI 的导出名称替代签名。 */
static int level_contract(logger_t *owner, int cold)
{
	int (*set_instance)(logger_t *, logger_level_t) =
		logger_set_instance_level;
	int (*set_default)(logger_level_t) = logger_set_level;
	errno = EDOM;
	if (cold) {
		if (set_instance((logger_t *)(uintptr_t)1, LOGGER_INFO) !=
			    -ENODEV ||
		    errno != EDOM || set_default(LOGGER_INFO) != -ENODEV ||
		    errno != EDOM)
			return 1;
		return 0;
	}
	logger_diagnostics_t diag;
	if (set_instance(owner, LOGGER_DEBUG) || errno != EDOM)
		return 2;
	if (logger_get_diagnostics(owner, &diag) || errno != EDOM ||
	    diag.level != LOGGER_DEBUG)
		return 3;
	if (set_default(LOGGER_WARN) || errno != EDOM)
		return 4;
	if (logger_get_global_diagnostics(&diag) || errno != EDOM ||
	    diag.level != LOGGER_WARN)
		return 5;
	if (set_instance(NULL, LOGGER_INFO) != -EINVAL || errno != EDOM)
		return 6;
	if (set_default((logger_level_t)-1) != -EINVAL || errno != EDOM)
		return 6;
	logger_level_t invalid = (logger_level_t)(LOGGER_OFF + 1);
	if (set_instance(owner, invalid) != -EINVAL || errno != EDOM)
		return 6;
	if (logger_get_diagnostics(owner, &diag) || errno != EDOM ||
	    diag.level != LOGGER_WARN)
		return 7;
	if (set_default(LOGGER_INFO) || errno != EDOM)
		return 8;
	return 0;
}

static int defaults_match(void)
{
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	if (l.struct_size != 144 || l.version != 1 || l.level != LOGGER_INFO ||
	    l.detail != LOGGER_DETAIL_VERBOSE ||
	    l.outputs != LOGGER_OUT_STDERR || l.file_path != NULL ||
	    l.rotation.mode != LOGGER_ROTATE_SIZE_DAILY ||
	    l.rotation.max_file_size != 100u * 1024u * 1024u ||
	    l.rotation.retention_days != 30 || l.file_mode != 0640 ||
	    l.async_mode != 1 || l.queue_capacity != 8192 ||
	    l.flush_level != LOGGER_ERROR || l.include_pid != 1 ||
	    l.include_tid != 1 || l.include_source != 1 || !l.ident ||
	    strcmp(l.ident, "app"))
		return 1;
	const logger_overflow_policy_t overflow[6] = {
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,
		LOGGER_OVERFLOW_DROP, LOGGER_OVERFLOW_DROP,
		LOGGER_OVERFLOW_SYNC, LOGGER_OVERFLOW_SYNC
	};
	for (unsigned i = 0; i < 6; ++i)
		if (l.overflow[i] != overflow[i])
			return 2;
	if (l.syslog.path != NULL ||
	    l.syslog.facility != LOGGER_SYSLOG_FACILITY_USER ||
	    l.syslog.reconnect_interval_ms != 1000u ||
	    l.syslog.startup != LOGGER_SYSLOG_START_REQUIRED)
		return 3;

	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.struct_size != 16 || c.version != 1 ||
	    c.verbosity != CONSOLE_NORMAL || c.color != CONSOLE_COLOR_AUTO)
		return 5;
	return 0;
}

int main(void)
{
	int rc = defaults_match();
	if (rc)
		return rc;
	/* 真实安装库的 C/C++11 调用，不仅检查头文件中存在声明。 */
	logger_config_t cold_config = LOGGER_DEFAULT_CONFIG();
	logger_t *cold_owner = NULL;
	errno = EDOM;
	if (logger_create(&cold_config, &cold_owner) != -ENODEV ||
	    cold_owner != NULL || errno != EDOM)
		return 16;
	if (logger_destroy(&cold_owner) != -ENODEV || errno != EDOM)
		return 17;
	if (logger_flush(NULL) != -ENODEV || errno != EDOM ||
	    logger_reopen(NULL) != -ENODEV || errno != EDOM ||
	    logger_default_flush() != -ENODEV || errno != EDOM ||
	    logger_default_reopen() != -ENODEV || errno != EDOM)
		return 21;
	if (record_query_contract(NULL, 1))
		return 28;
	if (context_contract(1))
		return 30;
	if (level_contract(NULL, 1))
		return 32;
	/* Console 配置返回状态，TTY 用独立输出槽，不再混淆错误与 false。 */
	console_config_t console = CONSOLE_DEFAULT_CONFIG();
	int (*configure_console)(const console_config_t *) = console_init;
	int (*set_color)(console_color_mode_t) = console_set_color;
	int (*set_verbosity)(console_verbosity_t) = console_set_verbosity;
	int (*is_stdout_tty)(int *) = console_stdout_is_tty;
	int (*is_stderr_tty)(int *) = console_stderr_is_tty;
	int tty = 7;
	errno = EDOM;
	if (configure_console(&console) != -ENODEV || errno != EDOM ||
	    is_stdout_tty(&tty) != -ENODEV || tty != 7 || errno != EDOM)
		return 23;
	int (*initialize)(void) = logger_process_init;
	errno = EDOM;
	if (initialize() != 0 || errno != EDOM)
		return 10;
	if (initialize() != -EALREADY || errno != EDOM)
		return 11;
	if (context_contract(0))
		return 31;
	if (configure_console(&console) || errno != EDOM ||
	    set_color(CONSOLE_COLOR_NEVER) || errno != EDOM ||
	    set_verbosity(CONSOLE_QUIET) || errno != EDOM)
		return 24;
	if (configure_console(NULL) != -EINVAL || errno != EDOM ||
	    set_color((console_color_mode_t)-1) != -EINVAL || errno != EDOM ||
	    is_stdout_tty(NULL) != -EINVAL || errno != EDOM)
		return 25;
	/* 真实安装消费环境中的输出描述符必须有效，答案可以是终端或非终端。 */
	if (is_stdout_tty(&tty) || (tty != 0 && tty != 1) || errno != EDOM ||
	    is_stderr_tty(&tty) || (tty != 0 && tty != 1) || errno != EDOM)
		return 26;
	int (*print_console)(const char *, ...) = console_print;
	if (print_console(NULL) != -EINVAL || errno != EDOM ||
	    console_info("filtered-installed-console") || errno != EDOM)
		return 27;
	int (*create)(const logger_config_t *, logger_t **) = logger_create;
	int (*destroy)(logger_t **) = logger_destroy;
	logger_t *owner = NULL;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.async_mode = 0;
	if (create(&cfg, &owner) || !owner || errno != EDOM)
		return 12;
	if (create(&cfg, &owner) != -EBUSY || !owner || errno != EDOM)
		return 13;
	int (*set_default)(logger_t *, logger_t *) = logger_default_set;
	if (set_default(NULL, owner) || errno != EDOM)
		return 18;
	if (record_query_contract(owner, 0))
		return 29;
	if (level_contract(owner, 0))
		return 33;
	/* 安装后的 C/C++ 使用全部四个新控制签名；stderr 无文件可重开。 */
	int (*flush)(logger_t *) = logger_flush;
	int (*reopen)(logger_t *) = logger_reopen;
	int (*default_flush)(void) = logger_default_flush;
	int (*default_reopen)(void) = logger_default_reopen;
	if (flush(owner) || errno != EDOM || default_flush() || errno != EDOM ||
	    reopen(owner) != -EINVAL || errno != EDOM ||
	    default_reopen() != -EINVAL || errno != EDOM)
		return 22;
	if (destroy(&owner) != -EBUSY || !owner || errno != EDOM)
		return 19;
	if (set_default(owner, NULL) || errno != EDOM)
		return 20;
	if (destroy(&owner) || owner != NULL || errno != EDOM)
		return 14;
	if (destroy(&owner) || owner != NULL || errno != EDOM)
		return 15;
	return 0;
}
