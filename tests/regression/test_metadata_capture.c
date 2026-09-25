#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include "support.h"
#include <unistd.h>

static logger_t *make_logger(logger_detail_t detail, int include_pid,
			     int include_tid, int include_source)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "/dev/null";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = 0;
	c.detail = detail;
	c.include_pid = include_pid;
	c.include_tid = include_tid;
	c.include_source = include_source;
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	return l;
}

static void check_mask(logger_detail_t detail, int include_pid, int include_tid,
		       int include_source, unsigned expected)
{
	logger_t *l =
		make_logger(detail, include_pid, include_tid, include_source);
	CHECK(l->capture_mask == expected);
	if (expected & LOGGER_CAPTURE_PID)
		CHECK(l->pid == getpid());
	else
		CHECK(l->pid == 0);
	CHECK(logger_destroy_status(l) == 0);
}

int main(void)
{
	check_mask(LOGGER_DETAIL_MINIMAL, 1, 1, 1, 0);
	check_mask(LOGGER_DETAIL_NORMAL, 1, 1, 1, LOGGER_CAPTURE_MODULE);
	check_mask(LOGGER_DETAIL_VERBOSE, 1, 1, 1,
		   LOGGER_CAPTURE_MODULE | LOGGER_CAPTURE_PID |
			   LOGGER_CAPTURE_TID);
	check_mask(LOGGER_DETAIL_VERBOSE, 0, 0, 1, LOGGER_CAPTURE_MODULE);
	check_mask(LOGGER_DETAIL_DEBUG, 0, 0, 0,
		   LOGGER_CAPTURE_MODULE | LOGGER_CAPTURE_CONTEXT);
	check_mask(LOGGER_DETAIL_DEBUG, 1, 1, 1,
		   LOGGER_CAPTURE_MODULE | LOGGER_CAPTURE_CONTEXT |
			   LOGGER_CAPTURE_SOURCE | LOGGER_CAPTURE_PID |
			   LOGGER_CAPTURE_TID);

	puts("demand-driven metadata capture mask matches detail/include policy");
	return 0;
}
