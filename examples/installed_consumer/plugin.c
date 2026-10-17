#include <logger.h>
/* 两个入口属于 SDK 的 ABI；宿主先初始化此 DSO 的 Logger 副本。
 * 不由构造函数或首次 plugin_run 隐式接管宿主进程生命周期。
 */
int plugin_process_init(void)
{
	return logger_process_init();
}

int plugin_run(void)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "installed-plugin.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 16;
	logger_t *p = NULL;
	int create_rc = logger_create(&cfg, &p);
	if (create_rc)
		return 1;
	int record_rc = LOGGER_INFO(p, "plugin",
				   "SDK owns local logger and destroys before dlclose");
	int destroy_rc = logger_destroy(&p);
	return record_rc || destroy_rc ? 2 : 0;
}
