#include <logger.h>
#include <console.h>
#include <logger_version.h>
int main()
{
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
	logger_log_source(log, LOGGER_INFO, source, "C++11 caller using C ABI");
	return logger_destroy(&log) ? 3 : 0;
}
