/* 仅供测试的替代实现：即使目录里有旧归档，故意损坏的实现也不得通过
 * 与生产库完全相同的轮转测试。 */
#include "logger.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char active[256];
static int token;
static logger_t *bound;

int logger_process_init(void)
{
	return 0;
}

int logger_create(const logger_config_t *config, logger_t **out)
{
	int n = snprintf(active, sizeof(active), "%s", config->file_path);
	if (n < 0 || (size_t)n >= sizeof(active))
		return -EOVERFLOW;
	*out = (logger_t *)&token;
	return 0;
}

int logger_default_set(logger_t *expected, logger_t *replacement)
{
	if (bound != expected)
		return -ESTALE;
	bound = replacement;
	return 0;
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

int logger_destroy(logger_t **owner)
{
	if (bound)
		return -EBUSY;
	*owner = NULL;
	const char *mode = getenv("LOGGER_ROTATION_MUTATION");
	if (!mode || !strcmp(mode, "no-output"))
		return 0;
	if (!strcmp(mode, "shutdown-error")) {
		return -EIO;
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
