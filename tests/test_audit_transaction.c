#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	unlink("./audittxn.audit.log");
	unlink("./audittxn.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "audittxn";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.fsync_each_record = 1;
	c.failure_policy = AUDIT_FAIL_DENY;
	if (audit_init(&c)) {
		perror("audit_init");
		return 1;
	}
	audit_event_t e = { .event = "KEY_DELETE",
			    .actor = "admin",
			    .source = "local",
			    .resource = "key-42",
			    .operation = "delete_key" };
	if (audit_begin(&e)) {
		perror("audit_begin");
		return 2;
	}
	/* protected operation would run here */
	if (audit_end(&e, AUDIT_SUCCESS, 0)) {
		perror("audit_end");
		return 3;
	}
	audit_shutdown();
	FILE *f = fopen("./audittxn.audit.log", "r");
	if (!f)
		return 4;
	char b[16384];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	if (!strstr(b, "txn=1 phase=ATTEMPT event=\"KEY_DELETE\""))
		return 5;
	if (!strstr(b, "txn=1 phase=RESULT event=\"KEY_DELETE\""))
		return 6;
	if (!strstr(b, "result=SUCCESS error=0"))
		return 7;
	return 0;
}
