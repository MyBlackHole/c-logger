#ifndef EXAMPLE_SDK_H
#define EXAMPLE_SDK_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* This is an example business ABI, not another Logger API. The business .so
 * has NO binary dependency on liblogger and owns NO logger, thread or fd. */
typedef struct example_sdk example_sdk_t;
typedef enum {
	EXAMPLE_LOG_DEBUG,
	EXAMPLE_LOG_INFO,
	EXAMPLE_LOG_ERROR
} example_log_level_t;
typedef struct {
	example_log_level_t level;
	const char *module;
	const char *file;
	const char *function;
	int line;
	const char *message; /* borrowed during callback, UTF-8/text, no NUL */
	size_t message_len;
} example_log_record_t;
typedef void (*example_log_fn)(void *user, const example_log_record_t *record);
typedef struct {
	const char *name; /* copied by example_sdk_create */
	example_log_fn log; /* NULL disables diagnostic logs, no fallback */
	void *log_user; /* borrowed; example_sdk_destroy NEVER frees it */
} example_sdk_options_t;
int example_sdk_create(const example_sdk_options_t *, example_sdk_t **out);
/* Concurrent run calls are allowed. Logging is synchronous to the caller,
 * with no business lock held. The callback must be thread-safe; no C++ exception
 * or longjmp may escape it. It must not destroy the active SDK instance. */
int example_sdk_run(example_sdk_t *, unsigned task);
/* Owner stops/joins ALL callers first. No more callbacks after destroy returns.
 * Real asynchronous SDKs also need to stop/join their own work here. */
void example_sdk_destroy(example_sdk_t *);
#ifdef __cplusplus
}
#endif
#endif
