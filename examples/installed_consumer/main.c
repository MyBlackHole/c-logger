#include <logger.h>
#include <console.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	char masked[16] = "abcd";
	int (*mask)(const char *input, char *out, size_t cap,
		    size_t keep_prefix,
		    size_t keep_suffix) = logger_mask_secret;

	errno = EDOM;
	if (mask(masked, masked, sizeof(masked), 1, 1) || errno != EDOM)
		return 40;
	if (strcmp(masked, "a***d"))
		return 41;
	if (mask(NULL, masked, 2, 0, 0) != -ENOSPC || errno != EDOM)
		return 42;
	if (strcmp(masked, "a***d"))
		return 43;
	if (logger_process_init() != 0)
		return 1;
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.outputs = LOGGER_OUT_FILE;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 64;
	c.file_path = "installed-a.log";
	logger_t *a = NULL;
	int create_rc = logger_create(&c, &a);

	if (create_rc) {
		errno = -create_rc;
		perror("logger_create(a)");
		return 1;
	}
	c.file_path = "installed-b.log";
	c.async_mode = 0;
	logger_t *b = NULL;

	create_rc = logger_create(&c, &b);
	if (create_rc) {
		errno = -create_rc;
		perror("logger_create(b)");
		logger_destroy(&a);
		return 2;
	}
	int rc = 0;

	for (unsigned int i = 0; i < 16; ++i) {
		int record_rc = LOGGER_INFO(a, "host-a", "record=%u", i);

		if (record_rc && !rc)
			rc = record_rc;
		record_rc = LOGGER_INFO(b, "host-b", "record=%u", i);
		if (record_rc && !rc)
			rc = record_rc;
	}
	int flush_rc = logger_flush(a);

	if (!rc)
		rc = flush_rc;
	logger_io_metrics_t io;
	int io_rc = logger_get_io_metrics(a, &io);
	logger_diagnostics_t diagnostics;
	int diagnostics_rc = logger_get_diagnostics(a, &diagnostics);
	logger_file_metrics_t file_metrics;
	int file_rc = logger_get_file_metrics(a, &file_metrics);

	if (io_rc || diagnostics_rc || file_rc) {
		if (!rc)
			rc = io_rc	    ? io_rc :
			     diagnostics_rc ? diagnostics_rc :
					      file_rc;
	} else if (io.emitted_records != 16 || io.failed_records ||
		   diagnostics.state != LOGGER_STATE_RUNNING ||
		   !diagnostics.async_mode ||
		   diagnostics.emitted_records != 16 ||
		   diagnostics.failed_records || diagnostics.queue_depth ||
		   diagnostics.completion_backlog ||
		   !file_metrics.write_operations ||
		   file_metrics.failed_write_operations ||
		   !file_metrics.bytes_written ||
		   !file_metrics.data_sync_attempts || file_metrics.last_error)
		rc = -EIO;
	int close_b = logger_destroy(&b);
	int close_a = logger_destroy(&a);

	if (!rc && (close_b || close_a))
		rc = close_b ? close_b : close_a;
	if (a || b)
		rc = -EIO;
	if (rc) {
		errno = -rc;
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
