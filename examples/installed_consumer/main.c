#include <logger.h>
#include <console.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 64;
	c.file_path = "installed-a.log";
	logger_t *a = logger_create(&c);
	if (!a) {
		perror("logger_create(a)");
		return 1;
	}
	c.file_path = "installed-b.log";
	c.async_mode = 0;
	logger_t *b = logger_create(&c);
	if (!b) {
		perror("logger_create(b)");
		logger_destroy(a);
		return 2;
	}
	for (unsigned i = 0; i < 16; ++i) {
		LOGGER_INFO(a, "host-a", "record=%u", i);
		LOGGER_INFO(b, "host-b", "record=%u", i);
	}
	int rc = logger_flush_instance_status(a);
	logger_io_metrics_t io;
	logger_get_io_metrics(a, &io);
	logger_diagnostics_t diagnostics;
	logger_get_diagnostics(a, &diagnostics);
	logger_file_metrics_t file_metrics;
	if (logger_get_file_metrics(a, &file_metrics))
		rc = -1;
	if (io.emitted_records != 16 || io.failed_records ||
	    diagnostics.state != LOGGER_STATE_RUNNING ||
	    !diagnostics.async_mode || diagnostics.emitted_records != 16 ||
	    diagnostics.failed_records || diagnostics.queue_depth ||
	    diagnostics.completion_backlog || !file_metrics.write_operations ||
	    file_metrics.failed_write_operations || !file_metrics.bytes_written ||
	    !file_metrics.data_sync_attempts || file_metrics.last_error)
		rc = -1;
	if (logger_destroy_status(b))
		rc = -1;
	if (logger_destroy_status(a))
		rc = -1;
	if (rc) {
		perror("logger drain");
		return 3;
	}

	console_config_t cc = CONSOLE_DEFAULT_CONFIG();
	cc.color = CONSOLE_COLOR_NEVER;
	console_init(&cc);
	if (console_print("installed Logger/Console consumer passed"))
		return 8;
	return 0;
}
