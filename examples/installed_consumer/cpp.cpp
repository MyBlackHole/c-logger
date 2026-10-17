#include <logger.h>
#include <console.h>
#include <logger_version.h>
#include <errno.h>
#include <string.h>
int main()
{
	char masked[16] = "abcd";
	int (*mask)(const char *, char *, size_t, size_t, size_t) =
		logger_mask_secret;

	errno = EDOM;
	if (mask(masked, masked, sizeof(masked), 1, 1) || errno != EDOM)
		return 40;
	if (strcmp(masked, "a***d"))
		return 41;
	if (mask(NULL, masked, 2, 0, 0) != -ENOSPC || errno != EDOM)
		return 42;
	if (strcmp(masked, "a***d"))
		return 43;
	if (logger_process_init() != 0)
		return 1;
	auto cfg = LOGGER_DEFAULT_CONFIG();
	const auto console = CONSOLE_DEFAULT_CONFIG();
	const auto source = LOGGER_SOURCE();
	if (cfg.struct_size != sizeof(cfg) ||
	    console.struct_size != sizeof(console) || source.line <= 0 ||
	    LOGGER_ABI_VERSION != EXPECTED_LOGGER_ABI)
		return 1;
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "installed-cpp.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = 0;
	logger_t *log = NULL;
	int create_rc = logger_create(&cfg, &log);
	if (create_rc)
		return 2;
	int (*record)(logger_t *, logger_level_t, logger_source_t, const char *,
		      ...) = logger_log_source;
	int (*state_query)(const logger_t *, logger_state_t *) =
		logger_get_state;
	int (*dropped_query)(const logger_t *, uint64_t *) = logger_dropped;
	logger_state_t state = LOGGER_STATE_CREATED;
	uint64_t dropped = 7;
	errno = EDOM;
	int rc = record(log, LOGGER_INFO, source, "C++11 caller using C ABI");
	if (rc || errno != EDOM || state_query(log, &state) ||
	    state != LOGGER_STATE_RUNNING || errno != EDOM ||
	    dropped_query(log, &dropped) || dropped != 0 || errno != EDOM)
		rc = 1;
	int close_rc = logger_destroy(&log);
	return rc || close_rc ? 3 : 0;
}
