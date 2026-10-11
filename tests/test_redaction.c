#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	const char *secret = "super-secret-token-12345";
	char b[64];
	if (strcmp(logger_redact(), "[REDACTED]"))
		return 1;
	if (logger_mask_secret(secret, b, sizeof(b), 4, 4))
		return 2;
	if (strstr(b, secret))
		return 3;
	if (strcmp(b, "supe***2345"))
		return 4;
	if (logger_mask_secret("abc", b, sizeof(b), 3, 0))
		return 5;
	if (strcmp(b, "***"))
		return 6; /* helper refuses full disclosure */
	errno = 0;
	char tiny[3];
	if (logger_mask_secret(secret, tiny, sizeof(tiny), 0, 0) == 0 ||
	    errno != ENOSPC)
		return 7;

	unlink("./redact.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./redact.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = 0;
	logger_t *logger = logger_create(&c);
	if (!logger)
		return 8;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "auth", NULL, 0,
					NULL, "credential=%s", logger_redact());
	int close_rc = logger_destroy_status(logger);
	if (rc || close_rc)
		return 9;
	FILE *f = fopen("./redact.log", "r");
	if (!f)
		return 10;
	char data[32768];
	size_t n = fread(data, 1, sizeof(data) - 1, f);
	fclose(f);
	data[n] = 0;
	if (!strstr(data, "[REDACTED]"))
		return 11;
	if (strstr(data, secret))
		return 12;
	return 0;
}
