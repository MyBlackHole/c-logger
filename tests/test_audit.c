#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	unlink("./auditcase.audit.log");
	unlink("./auditcase.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "auditcase";
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1024 * 1024;
	c.fsync_each_record = 1;
	if (audit_init(&c)) {
		perror("audit_init");
		return 1;
	}
	audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
			    .transaction_id = 1,
			    .event = "KEY_DELETE",
			    .actor = "admin user",
			    .source = "10.0.0.21",
			    .resource = "key-1024",
			    .operation = "delete_key",
			    .result = AUDIT_SUCCESS,
			    .error_code = 0,
			    .detail = "reason=\"rotation test\"\napproved" };
	if (audit_write(&e)) {
		perror("audit_write");
		return 2;
	}
	audit_shutdown();
	FILE *f = fopen("./auditcase.audit.log", "r");
	if (!f)
		return 3;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	if (!strstr(b, "event=\"KEY_DELETE\""))
		return 4;
	if (!strstr(b, "actor=\"admin user\""))
		return 5;
	if (!strstr(b, "detail=\"reason=\\\"rotation test\\\"\\napproved\""))
		return 6;
	return 0;
}
