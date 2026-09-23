#include <logger.h>
#include <audit.h>
#include <console.h>
#include <logger_version.h>
int main()
{
	auto cfg = LOGGER_DEFAULT_CONFIG();
	const auto audit = AUDIT_DEFAULT_CONFIG();
	const auto console = CONSOLE_DEFAULT_CONFIG();
	const auto source = LOGGER_SOURCE();
	if (cfg.struct_size != sizeof(cfg) ||
	    audit.struct_size != sizeof(audit) ||
	    console.struct_size != sizeof(console) || source.line <= 0 ||
	    LOGGER_ABI_VERSION != 0)
		return 1;
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "installed-cpp.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = 0;
	logger_t *log = logger_create(&cfg);
	if (!log)
		return 2;
	logger_log_source(log, LOGGER_INFO, source, "C++11 caller using C ABI");
	return logger_destroy_status(log) ? 3 : 0;
}
