#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int gethash(char h[65])
{
	FILE *f = fopen("./restart.audit.state", "r");
	if (!f)
		return -1;
	char b[512];
	if (!fgets(b, sizeof(b), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	char *x = strstr(b, " hash=");
	if (!x)
		return -1;
	memcpy(h, x + 6, 64);
	h[64] = 0;
	return 0;
}
int main(void)
{
	unlink("./restart.audit.log");
	unlink("./restart.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "restart";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 1;
	audit_shutdown();
	char anchor[65];
	if (gethash(anchor))
		return 2;
	if (audit_init(&c))
		return 3;
	audit_shutdown();
	FILE *g = fopen("./restart.audit.log", "r");
	if (!g)
		return 4;
	char line[16384], last_start[16384] = { 0 };
	while (fgets(line, sizeof(line), g))
		if (strstr(line, "event=\"AUDIT_START\""))
			snprintf(last_start, sizeof(last_start), "%s", line);
	fclose(g);
	char needle[80];
	snprintf(needle, sizeof(needle), "prev=%s", anchor);
	if (!strstr(last_start, needle))
		return 5;
	return 0;
}
