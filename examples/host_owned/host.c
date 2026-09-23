#include "logger_adapter.h"
#include "plugin_api.h"
#include <errno.h>
#include <stdio.h>

int main(int argc, char **argv)
{
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
	cfg.async_mode = 1; /* HOST explicitly chooses background logging. */
	cfg.queue_capacity = 1024;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	logger = logger_create(&cfg);
	if (!logger) {
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
	/* This example has no business threads. A real host stops/joins callers
     * FIRST. Plugin instances borrow the logger and never destroy it. */
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
	/* Logger library and host callback code remain loaded until after destroy. */
	if (logger_destroy_status(logger)) {
		perror("logger_destroy_status");
		result = 1;
	}
	logger = NULL; /* freed even on a normal teardown I/O error */
	return result;
}
