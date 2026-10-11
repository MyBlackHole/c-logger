#include <logger.h>
/* 此函数是 SDK 的 ABI，不属于 Logger。 */
int plugin_run(void)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "installed-plugin.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 16;
	logger_t *p = logger_create(&cfg);
	if (!p)
		return 1;
	LOGGER_INFO(p, "plugin",
		    "SDK owns local logger and destroys before dlclose");
	return logger_destroy_status(p) ? 2 : 0;
}
