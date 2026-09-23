#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include "support.h"
int main(void)
{
	char dir[] = "/tmp/logger-global-flush-XXXXXX";
	enter_temp(dir);
	CHECK(logger_flush_instance_status(NULL) == -1 && errno == EINVAL);
	CHECK(logger_flush_status() == -1 && errno == ENODEV);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(logger_init(&c) == 0);
	LOG_INFO("global-before-flush");
	CHECK(logger_flush_status() == 0);
	CHECK(file_contains("out.log", "global-before-flush"));
	logger_io_metrics_t io;
	logger_get_global_io_metrics(&io);
	CHECK(io.async_completed == 1 && io.emitted_records == 1 &&
	      io.failed_records == 0);
	logger_shutdown();
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
	logger_get_global_io_metrics(&io);
	CHECK(io.async_completed == 0 && io.emitted_records == 0 &&
	      io.first_error == 0);
	leave_temp(dir);
	puts("global flush returns explicit status and observes queued output");
	return 0;
}
