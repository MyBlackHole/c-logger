#include <logger.h>
#include <console.h>
#include <logger_version.h>
#include <errno.h>
#include <string.h>
/* 由搬迁后的独立 C/C++11 消费者执行，只依赖安装的公开接口。 */
static int mask_contract(void)
{
	int (*mask)(const char *, char *, size_t, size_t, size_t) =
		logger_mask_secret;
	char value[32] = "abcdef";
	char tiny[3] = { 'X', 'Y', 'Z' };
	errno = EDOM;
	if (mask(value, value, sizeof(value), 1, 4) || errno != EDOM)
		return 1;
	if (strcmp(value, "a***cdef"))
		return 2;
	if (mask("secret", tiny, sizeof(tiny), 0, 0) != -ENOSPC)
		return 3;
	if (errno != EDOM || memcmp(tiny, "XYZ", sizeof(tiny)))
		return 4;
	if (mask(NULL, value, sizeof(value), 0, 0) || errno != EDOM)
		return 5;
	if (strcmp(value, logger_redact()))
		return 6;
	return 0;
}

int main()
{
	if (mask_contract())
		return 30;
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
