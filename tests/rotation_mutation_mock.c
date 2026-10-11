/* Test-only replacement: deliberately broken logger implementations must not
 * pass the exact production rotation test, even beside stale archives. */
#include "logger.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char active[256];

int logger_init(const logger_config_t *config)
{
	int n = snprintf(active, sizeof(active), "%s", config->file_path);
	return n < 0 || (size_t)n >= sizeof(active) ? -1 : 0;
}

void logger_global_write(logger_level_t level, const char *module,
			 const char *file, int line, const char *function,
			 const char *format, ...)
{
	(void)level;
	(void)module;
	(void)file;
	(void)line;
	(void)function;
	(void)format;
}

int logger_shutdown_status(void)
{
	const char *mode = getenv("LOGGER_ROTATION_MUTATION");
	if (!mode || !strcmp(mode, "no-output"))
		return 0;
	if (!strcmp(mode, "shutdown-error")) {
		errno = EIO;
		return -1;
	}
	char archive[300];
	int n = snprintf(archive, sizeof(archive), "%.*s.fixtureZ.log",
			 (int)strlen(active) - 4, active);
	if (n < 0 || (size_t)n >= sizeof(archive))
		return -1;
	FILE *files[2] = { fopen(active, "w"), NULL };
	if (strcmp(mode, "active-only"))
		files[1] = fopen(archive, "w");
	if (!files[0] || (strcmp(mode, "active-only") && !files[1])) {
		if (files[0])
			fclose(files[0]);
		if (files[1])
			fclose(files[1]);
		return -1;
	}
	int rc = 0;
	for (int i = 0; i < 100; i++) {
		if (i == 50 && !strcmp(mode, "missing"))
			continue;
		int id = i == 50 && !strcmp(mode, "duplicate") ? 49 : i;
		if (!strcmp(mode, "reordered"))
			id = i == 50 ? 51 : (i == 51 ? 50 : i);
		if (!strcmp(mode, "reversed-files"))
			id = (i + 50) % 100;
		const char *payload =
			i == 50 && !strcmp(mode, "corrupt") ?
				"corrupt" :
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		FILE *file = files[1] && i < 50 &&
					     strcmp(mode, "empty-archive") ?
				     files[1] :
				     files[0];
		if (fprintf(file, "fixture record=%d %s\n", id, payload) < 0)
			rc = -1;
	}
	if (fclose(files[0]))
		rc = -1;
	if (files[1] && fclose(files[1]))
		rc = -1;
	return rc;
}
