#include "audit.h"
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

	unlink("./redact.audit.log");
	unlink("./redact.audit.state");
	unlink("./redact.audit.lock");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "redact";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 8;
	audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
			    .event = "AUTH",
			    .actor = "user-42",
			    .source = "cli",
			    .resource = "credential",
			    .operation = "authenticate",
			    .result = AUDIT_SUCCESS,
			    .detail = AUDIT_DETAIL_REDACTED };
	if (audit_write(&e))
		return 9;
	audit_shutdown();
	FILE *f = fopen("./redact.audit.log", "r");
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
