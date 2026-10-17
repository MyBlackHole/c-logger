#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include "default_fixture.h"
static logger_t *default_owner;
#include "logger.h"
#include "support.h"
int main(void)
{
	CHECK(logger_process_init() == 0);
	char dir[] = "/tmp/logger-global-flush-XXXXXX";
	enter_temp(dir);
	CHECK(logger_flush(NULL) == -EINVAL);
	CHECK(logger_default_flush() == -ENODEV);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(fixture_default_create(&c, &default_owner) == 0);
	LOG_INFO("global-before-flush");
	CHECK(logger_default_flush() == 0);
	CHECK(file_contains("out.log", "global-before-flush"));
	logger_io_metrics_t io;
	CHECK(logger_get_global_io_metrics(&io) == 0);
	CHECK(io.async_completed == 1 && io.emitted_records == 1 &&
	      io.failed_records == 0);
	CHECK(fixture_default_destroy(&default_owner) == 0);
	CHECK(logger_default_flush() == -ENODEV);
	CHECK(logger_get_global_io_metrics(&io) == 0);
	CHECK(io.async_completed == 0 && io.emitted_records == 0 &&
	      io.first_error == 0);
	leave_temp(dir);
	puts("global flush returns explicit status and observes queued output");
	return 0;
}
