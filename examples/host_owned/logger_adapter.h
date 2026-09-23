#ifndef EXAMPLE_LOGGER_ADAPTER_H
#define EXAMPLE_LOGGER_ADAPTER_H
#include "logger.h"
#include "example_sdk.h"
#include <limits.h>

/* Compile into the HOST, not the SDK. Callback user is a borrowed logger_t *.
 * Ordinary logger_log is best effort; use owner-side metrics/flush for errors.
 * It is NOT an Audit commit acknowledgement. */
static inline void example_to_logger(void *user, const example_log_record_t *r)
{
	if (!user || !r || !r->message || r->message_len > INT_MAX)
		return;
	logger_level_t level;
	switch (r->level) {
	case EXAMPLE_LOG_DEBUG:
		level = LOGGER_DEBUG;
		break;
	case EXAMPLE_LOG_INFO:
		level = LOGGER_INFO;
		break;
	case EXAMPLE_LOG_ERROR:
		level = LOGGER_ERROR;
		break;
	default:
		return;
	}
	/* Treat the message as DATA. Never use a plugin's message as printf fmt. */
	logger_log((logger_t *)user, level, r->module, r->file, r->line,
		   r->function, "%.*s", (int)r->message_len, r->message);
}
#endif
