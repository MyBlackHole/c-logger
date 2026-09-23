#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	const char *p = "./chain.audit.log";
	unlink(p);
	unlink("./chain.audit.state");
	unlink("./chain.audit.lock");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "chain";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.integrity = AUDIT_INTEGRITY_SHA256;
	if (audit_init(&c))
		return 1;
	audit_event_t e = { .event = "CONFIG_CHANGE",
			    .actor = "admin",
			    .source = "local",
			    .resource = "tls",
			    .operation = "set_tls" };
	if (audit_begin(&e) || audit_end(&e, AUDIT_SUCCESS, 0))
		return 2;
	audit_shutdown();
	if (audit_verify_file(p))
		return 3;
	FILE *f = fopen(p, "r+");
	if (!f)
		return 4;
	char b[32768];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	char *x = strstr(b, "CONFIG_CHANGE");
	if (!x) {
		fclose(f);
		return 5;
	}
	x[0] = 'X';
	rewind(f);
	fwrite(b, 1, n, f);
	fflush(f);
	fclose(f);
	if (audit_verify_file(p) == 0)
		return 6;
	return 0;
}
