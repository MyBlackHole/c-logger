#include "logger.h"
#include "logger_adapter.h"
#include "plugin_api.h"
#include <errno.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	if (logger_process_init() != 0)
		return 1;
	if (argc != 3) {
		fprintf(stderr,
			"usage: %s /absolute/path/libexample_sdk.so log-file\n",
			argv[0]);
		return 2;
	}
	example_plugin_t plugin;
	if (example_plugin_open(&plugin, argv[1])) {
		fprintf(stderr, "load SDK: %s\n", dlerror());
		return 1;
	}
	int result = 1;
	logger_t *logger = NULL;
	example_sdk_t *first = NULL, *second = NULL;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = argv[2];
	cfg.detail = LOGGER_DETAIL_DEBUG;
	cfg.async_mode = 1; /* 宿主显式选择后台日志。 */
	cfg.queue_capacity = 1024;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	int create_rc = logger_create(&cfg, &logger);
	if (create_rc) {
		errno = -create_rc;
		perror("logger_create");
		goto cleanup;
	}
	example_sdk_options_t options = { "backup", example_to_logger, logger };
	if (plugin.create(&options, &first)) {
		perror("create first SDK");
		goto cleanup;
	}
	options.name = "storage";
	if (plugin.create(&options, &second)) {
		perror("create second SDK");
		goto cleanup;
	}
	if (plugin.run(first, 1) || plugin.run(second, 2)) {
		perror("SDK run");
		goto cleanup;
	}
	result = 0;
cleanup:
	/* 此示例没有业务线程。真实宿主先停止并回收调用线程。
	 * 插件实例借用 logger，绝不销毁它。
	 */
	plugin.destroy(second);
	plugin.destroy(first);
	if (logger) {
		LOGGER_INFO(logger, "host",
			    "SDK instances stopped; logger still usable");
		if (logger_flush_instance_status(logger)) {
			perror("logger_flush_instance_status");
			result = 1;
		}
	}
	if (dlclose(plugin.handle)) {
		fprintf(stderr, "unload SDK: %s\n", dlerror());
		result = 1;
	}
	/* Logger 库与宿主回调代码保持加载，直到销毁之后。 */
	int destroy_rc = logger_destroy(&logger);
	if (destroy_rc) {
		errno = -destroy_rc;
		perror("logger_destroy");
		result = 1;
	}
	logger = NULL; /* 即使普通拆除 I/O 出错也已释放 */
	return result;
}
