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
	OP_COUNT
};
static const char *const operation_names[] = { "write",	  "flush", "flush-void",
					       "reopen",  "level", "dropped",
					       "metrics", "io" };
typedef struct {
	enum operation op;
	int rc, error;
	uint64_t value;
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	_Atomic int done;
} call_result_t;

static inline int stop_status(void)
{
#ifdef LOGGER_GLOBAL_BASELINE
	logger_shutdown();
	return 0;
#else
	return logger_shutdown_status();
#endif
}

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
	errno = 0;
	switch (r->op) {
	case OP_WRITE:
		LOG_INFO("delayed-record");
		break;
	case OP_FLUSH:
		r->rc = logger_flush_status();
		break;
	case OP_FLUSH_VOID:
		logger_flush();
		break;
	case OP_REOPEN:
		r->rc = logger_reopen();
		break;
	case OP_LEVEL:
		logger_set_level(LOGGER_OFF);
		break;
	case OP_DROPPED:
		r->value = logger_global_dropped();
		break;
	case OP_METRICS:
		logger_get_global_metrics(&r->metrics);
		break;
	case OP_IO:
		logger_get_global_io_metrics(&r->io);
		break;
	default:
		CHECK(!"bad operation");
	}
	r->error = errno;
	atomic_store(&r->done, 1);
}

static inline void expect_rejection(const call_result_t *r, int error)
{
	CHECK(r->error == error);
	if (r->op == OP_FLUSH || r->op == OP_REOPEN)
		CHECK(r->rc == -1);
	if (r->op == OP_DROPPED)
		CHECK(r->value == 0);
	if (r->op == OP_METRICS) {
		CHECK(!r->metrics.enqueued && !r->metrics.sync_fallbacks &&
		      !r->metrics.consumer_records &&
		      !r->metrics.queue_high_watermark);
		for (int i = 0; i < 6; ++i)
			CHECK(!r->metrics.dropped[i]);
	}
	if (r->op == OP_IO)
		CHECK(!r->io.async_completed && !r->io.sync_completed &&
		      !r->io.emitted_records && !r->io.failed_records &&
		      !r->io.first_error);
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
