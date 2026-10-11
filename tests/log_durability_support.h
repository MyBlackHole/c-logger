#ifndef LOGGER_DURABILITY_SUPPORT_H
#define LOGGER_DURABILITY_SUPPORT_H
#include "logger.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared by process-crash and VM-powercut probes. No application transaction
 * or journal format is involved: only already-acknowledged log records. */
static inline logger_config_t durability_config(const char *point)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "durable.log";
	c.async_mode = 0;
	c.rotation.mode = !strncmp(point, "file_after_", 11) ?
		LOGGER_ROTATE_SIZE : LOGGER_ROTATE_NONE;
	c.rotation.max_file_size = 512;
	c.rotation.retention_days = 0;
	return c;
}

static inline int durability_record(logger_t *l, const char *event, unsigned id)
{
	return logger_log_sync_status(l, LOGGER_INFO, "durability", NULL, 0,
				     NULL, "%s id=%u", event, id);
}

static inline int durability_baseline(logger_t *l)
{
	for (unsigned i = 0; i < 10; ++i)
		if (durability_record(l, "LOGGER_BASELINE", i))
			return -1;
	return 0;
}

/* Verify the identity of EVERY acknowledged record, not just a total count.
 * Unacknowledged writes can be present, absent or partial after the crash. */
static inline int durability_verify(void)
{
	DIR *d = opendir(".");
	if (!d)
		return -1;
	unsigned seen[10] = { 0 };
	int rc = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (strncmp(e->d_name, "durable", 7) || len < 4 ||
		    strcmp(e->d_name + len - 4, ".log"))
			continue;
		FILE *f = fopen(e->d_name, "r");
		if (!f) {
			rc = -1;
			break;
		}
		char line[8192];
		while (fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "LOGGER_BASELINE id=");
			if (!p)
				continue;
			unsigned id;
			char end;
			if (sscanf(p, "LOGGER_BASELINE id=%u%c", &id, &end) != 2 ||
			    end != '\n' || id >= 10 || ++seen[id] != 1)
				rc = -1;
		}
		if (ferror(f))
			rc = -1;
		if (fclose(f))
			rc = -1;
	}
	if (closedir(d))
		rc = -1;
	for (unsigned i = 0; i < 10; ++i)
		if (seen[i] != 1)
			rc = -1;
	return rc;
}
#endif
