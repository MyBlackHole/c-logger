#include "logger.h"
#include "console.h"
#include "logger_version.h"
#include <errno.h>
#include <string.h>

int main(void)
{
	_Static_assert(LOGGER_ABI_VERSION == 2, "expected Logger ABI 2");
	errno = 0;
	if (logger_create(NULL) != NULL || errno != EINVAL)
		return 1;
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_STDERR;
	c.async_mode = 0;
	logger_t *l = logger_create(&c);
	if (!l)
		return 2;
	int rc = logger_log_sync_status(l, LOGGER_INFO, "contract", NULL, 0,
					NULL, "API contract");
	logger_io_metrics_t io;
	logger_get_io_metrics(l, &io);
	if (logger_destroy_status(l) || rc || io.emitted_records != 1 ||
	    io.failed_records)
		return 3;
	logger_context_set(&(logger_context_t){ .request_id = "request-1" });
	logger_context_t ctx = logger_context_get();
	if (!ctx.request_id || strcmp(ctx.request_id, "request-1"))
		return 4;
	logger_context_clear();
	if (logger_context_get().request_id)
		return 5;
	return 0;
}
