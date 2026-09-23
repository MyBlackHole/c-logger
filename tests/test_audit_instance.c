#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int run(char id[33])
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "auditinstance";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 1;
	snprintf(id, 33, "%s", audit_instance_id());
	audit_event_t e = { .event = "LOGIN",
			    .actor = "admin",
			    .source = "local",
			    .resource = "cli",
			    .operation = "login" };
	if (audit_begin(&e))
		return 2;
	if (audit_end(&e, AUDIT_SUCCESS, 0))
		return 3;
	audit_shutdown();
	return 0;
}
int main(void)
{
	unlink("./auditinstance.audit.log");
	unlink("./auditinstance.audit.state");
	char a[33], b[33];
	if (run(a))
		return 1;
	if (run(b))
		return 2;
	if (strlen(a) != 32 || strlen(b) != 32 || strcmp(a, b) == 0)
		return 3;
	FILE *f = fopen("./auditinstance.audit.log", "r");
	if (!f)
		return 4;
	char buf[32768];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (!strstr(buf, "event=\"AUDIT_START\""))
		return 5;
	if (!strstr(buf, "event=\"AUDIT_STOP\""))
		return 6;
	if (!strstr(buf, a) || !strstr(buf, b))
		return 7;
	return 0;
}
