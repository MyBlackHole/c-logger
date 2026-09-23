/* Also compiled as C++11. Compare all public aggregate sizes/alignments,
 * selected offsets, enum values and all default initializer members. */
#include <logger.h>
#include <audit.h>
#include <console.h>
#include <stdio.h>
#ifdef __cplusplus
#define ALIGN(T) alignof(T)
#else
#define ALIGN(T) _Alignof(T)
#endif
#define TYPE(T) printf(#T " %zu %zu\n", sizeof(T), (size_t)ALIGN(T))
#define FIELD(T, F) printf(#T "." #F " %zu\n", offsetof(T, F))
#define N(F) printf(#F " %llu\n", (unsigned long long)(F))
int main(void)
{
	TYPE(logger_config_t);
	TYPE(logger_rotation_config_t);
	TYPE(logger_source_t);
	TYPE(logger_context_t);
	TYPE(logger_metrics_t);
	TYPE(logger_io_metrics_t);
	TYPE(logger_syslog_config_t);
	TYPE(logger_syslog_metrics_t);
	TYPE(audit_config_t);
	TYPE(audit_event_t);
	TYPE(audit_status_t);
	TYPE(console_config_t);
	FIELD(logger_config_t, version);
	FIELD(logger_config_t, file_path);
	FIELD(logger_config_t, rotation);
	FIELD(logger_config_t, overflow);
	FIELD(logger_config_t, syslog);
	FIELD(audit_config_t, rotation);
	FIELD(audit_config_t, integrity);
	FIELD(audit_event_t, event);
	FIELD(audit_event_t, detail);
	FIELD(audit_status_t, instance_id);
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	N(l.struct_size);
	N(l.version);
	N(l.level);
	N(l.detail);
	N(l.outputs);
	N(l.file_path == NULL);
	N(l.rotation.mode);
	N(l.rotation.max_file_size);
	N(l.rotation.retention_days);
	N(l.file_mode);
	N(l.async_mode);
	N(l.queue_capacity);
	N(l.flush_level);
	N(l.include_pid);
	N(l.include_tid);
	N(l.include_source);
	printf("ident=%s\n", l.ident);
	for (unsigned i = 0; i < 6; ++i)
		N(l.overflow[i]);
	N(l.syslog.path == NULL);
	N(l.syslog.facility);
	N(l.syslog.reconnect_interval_ms);
	N(l.syslog.startup);
	N(a.struct_size);
	N(a.version);
	printf("audit=%s/%s\n", a.log_dir, a.name);
	N(a.chain_state_path == NULL);
	N(a.rotation.mode);
	N(a.rotation.max_file_size);
	N(a.rotation.retention_days);
	N(a.failure_policy);
	N(a.integrity);
	N(a.fsync_each_record);
	N(c.struct_size);
	N(c.version);
	N(c.verbosity);
	N(c.color);
	return 0;
}
