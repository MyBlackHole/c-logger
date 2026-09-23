#include "audit.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	unlink("./rotchain.audit.log");
	unlink("./rotchain.audit.state");
	DIR *d = opendir(".");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)))
			if (strncmp(e->d_name, "rotchain.", 9) == 0 &&
			    (strstr(e->d_name, "Z.audit.log") ||
			     strstr(e->d_name, "Z.log")))
				unlink(e->d_name);
		closedir(d);
	}
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "rotchain";
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 900;
	c.rotation.retention_days = 30;
	if (audit_init(&c))
		return 1;
	for (int i = 0; i < 8; i++) {
		char r[64];
		snprintf(r, sizeof(r), "resource-%d", i);
		audit_event_t e = { .event = "UPDATE",
				    .actor = "admin",
				    .source = "local",
				    .resource = r,
				    .operation = "update" };
		if (audit_begin(&e) || audit_end(&e, AUDIT_SUCCESS, 0))
			return 2;
	}
	audit_shutdown();
	/* Normal restart must accept a checkpoint whose historical bytes may now live in an archive. */
	if (audit_init(&c))
		return 3;
	audit_shutdown();
	return 0;
}
