#ifndef AUDIT_REGRESSION_SUPPORT_H
#define AUDIT_REGRESSION_SUPPORT_H
#include "audit.h"
#include "support.h"
#include <dirent.h>

static inline audit_config_t audit_test_config(void)
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "app";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.failure_policy = AUDIT_FAIL_DENY;
	return c;
}

static inline audit_event_t audit_test_event(const char *name)
{
	audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
			    .event = name,
			    .actor = "user",
			    .source = "test",
			    .resource = "item",
			    .operation = "update",
			    .result = AUDIT_SUCCESS };
	return e;
}

static inline void audit_test_cleanup(const char *path)
{
	DIR *d = opendir(".");
	CHECK(d != NULL);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			CHECK(unlink(e->d_name) == 0);
	}
	CHECK(closedir(d) == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(path) == 0);
}

static inline unsigned count_event(const char *name)
{
	char marker[160];
	CHECK(snprintf(marker, sizeof(marker), " event=\"%s\"", name) > 0);
	FILE *f = fopen("app.audit.log", "r");
	CHECK(f != NULL);
	char line[8192];
	unsigned count = 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, marker))
			++count;
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return count;
}

#endif
