#ifndef GLOBAL_REGRESSION_SUPPORT_H
#define GLOBAL_REGRESSION_SUPPORT_H
#ifdef LOGGER_PUBLIC_ABI_TEST
#include "logger.h"
#else
#include "logger_internal.h"
#endif
#include "support.h"
#include <fcntl.h>

enum operation {
	OP_WRITE,
	OP_FLUSH,
	OP_FLUSH_VOID,
	OP_REOPEN,
	OP_LEVEL,
	OP_DROPPED,
	OP_METRICS,
	OP_IO,
	OP_DIAGNOSTICS,
	OP_FILE_METRICS,
	OP_COUNT
};
static const char *const operation_names[] = { "write",	  "flush", "flush-void",
					       "reopen",  "level", "dropped",
					       "metrics", "io", "diagnostics", "file-metrics" };
typedef struct {
	enum operation op;
	int rc, error;
	uint64_t value;
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_diagnostics_t diagnostics;
	logger_file_metrics_t file_metrics;
	_Atomic int done;
} call_result_t;

static inline logger_config_t config_for(const char *path, int async)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = path;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = async;
	cfg.queue_capacity = 64;
	return cfg;
}

static inline enum operation parse_operation(const char *name)
{
	for (int i = 0; i < OP_COUNT; ++i)
		if (!strcmp(name, operation_names[i]))
			return (enum operation)i;
	CHECK(!"unknown global operation");
	return OP_WRITE;
}

static inline void invoke(call_result_t *r)
{
	r->rc = 0;
	r->value = UINT64_C(777);
	memset(&r->metrics, 0xa5, sizeof(r->metrics));
	memset(&r->io, 0xa5, sizeof(r->io));
	memset(&r->diagnostics, 0xa5, sizeof(r->diagnostics));
	memset(&r->file_metrics, 0xa5, sizeof(r->file_metrics));
	errno = EDOM;
	switch (r->op) {
	case OP_WRITE:
		r->rc = LOG_INFO("delayed-record");
		break;
	case OP_FLUSH:
		r->rc = logger_default_flush();
		break;
	case OP_FLUSH_VOID:
		(void)(r->rc = logger_default_flush());
		break;
	case OP_REOPEN:
		r->rc = logger_default_reopen();
		break;
	case OP_LEVEL:
		logger_set_level(LOGGER_OFF);
		break;
	case OP_DROPPED:
		r->rc = logger_global_dropped(&r->value);
		break;
	case OP_METRICS:
		r->rc = logger_get_global_metrics(&r->metrics);
		break;
	case OP_IO:
		r->rc = logger_get_global_io_metrics(&r->io);
		break;
	case OP_DIAGNOSTICS:
		r->rc = logger_get_global_diagnostics(&r->diagnostics);
		break;
	case OP_FILE_METRICS:
		r->rc = logger_get_global_file_metrics(&r->file_metrics);
		break;
	default:
		CHECK(!"bad operation");
	}
	r->error = errno;
	atomic_store(&r->done, 1);
}

static inline void expect_query_sentinel(const void *out, size_t size)
{
	const unsigned char *bytes = out;
	for (size_t i = 0; i < size; ++i)
		CHECK(bytes[i] == 0xa5);
}

static inline void expect_rejection(const call_result_t *r, int error)
{
	/* 级别设置在本阶段保留旧 errno 合同；其他入口实际检查返回状态。 */
	if (r->op == OP_LEVEL) {
		CHECK(r->error == error);
		return;
	}
	CHECK(r->rc == -error && r->error == EDOM);
	if (r->op == OP_DROPPED)
		CHECK(r->value == UINT64_C(777));
	if (r->op == OP_METRICS)
		expect_query_sentinel(&r->metrics, sizeof(r->metrics));
	if (r->op == OP_IO)
		expect_query_sentinel(&r->io, sizeof(r->io));
	if (r->op == OP_DIAGNOSTICS)
		expect_query_sentinel(&r->diagnostics, sizeof(r->diagnostics));
	if (r->op == OP_FILE_METRICS)
		expect_query_sentinel(&r->file_metrics,
				      sizeof(r->file_metrics));
}

static inline void fixture_reset(void)
{
	const char *names[] = { "old.log", "new.log", "side.log",
				"stderr.log" };
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		(void)unlink(names[i]);
		char lock[64];
		CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", names[i]) >
		      0);
		(void)unlink(lock);
	}
}

static inline unsigned file_lines(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f);
	unsigned n = 0;
	int c;
	while ((c = fgetc(f)) != EOF)
		if (c == '\n')
			++n;
	CHECK(!ferror(f) && !fclose(f));
	return n;
}
#endif
