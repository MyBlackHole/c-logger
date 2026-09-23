#include "audit.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
	/*
     * Use name "../dev/full" is intentionally rejected by safe_name, so test
     * logger status directly for deterministic ENOSPC propagation.
     */
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "/dev/full";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.flush_level = LOGGER_INFO;
	logger_t *l = logger_create(&c);
	if (!l) {
		perror("logger_create");
		return 1;
	}
	int rc = logger_log_sync_status(l, LOGGER_INFO, "AUDIT", NULL, 0, NULL,
					"probe");
	logger_destroy(l);
	if (rc != -ENOSPC) {
		fprintf(stderr, "expected -ENOSPC, got %d\n", rc);
		return 2;
	}
	return 0;
}
