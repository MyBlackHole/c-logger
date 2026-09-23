#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include <fcntl.h>
static int fail_close, selected_fd = -1;
int __real_close(int);
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (fd == selected_fd && fail_close) {
		fail_close = 0;
		errno = EIO;
		return -1;
	}
	return rc;
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	unlink("dispose.log");
	unlink("next.log");
	if (!strcmp(argv[1], "null")) {
		CHECK(logger_destroy_status(NULL) == 0);
		return 0;
	}
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "dispose.log";
	c.async_mode = 1;
	c.queue_capacity = 2048;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	int io_error = !strcmp(argv[1], "io-error");
	if (io_error) {
		c.file_path = "/dev/full";
		c.async_mode = 0;
	}
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	if (!strcmp(argv[1], "close-error")) {
		selected_fd = l->file_backend.fd;
		fail_close = 1;
	}
	int expected = io_error ? ENOSPC : fail_close ? EIO : 0;
	for (int i = 0; i < (io_error ? 1 : 1000); ++i)
		LOGGER_INFO(l, "test", "record=%d", i);
	int rc = logger_destroy_status(l);
	l = NULL; /* never retry/free/use the destroyed pointer, including error */
	CHECK(expected ? rc == -1 && errno == expected : rc == 0);
	CHECK(logger_process_object_count() == 0);
	if (selected_fd >= 0)
		CHECK(fcntl(selected_fd, F_GETFD) == -1 && errno == EBADF);
	if (!io_error)
		CHECK(log_contains("dispose.log", "record=999"));
	c.file_path = "next.log";
	c.async_mode = 0;
	l = logger_create(&c);
	CHECK(l != NULL);
	CHECK(logger_destroy_status(l) == 0);
	return 0;
}
