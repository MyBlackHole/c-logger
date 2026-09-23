#ifndef LOGGER_RECORD_H
#define LOGGER_RECORD_H
#include "logger.h"
#include <sys/types.h>
#include <time.h>
#include <string.h>
#define LOGGER_MESSAGE_MAX 4096
#define LOGGER_LINE_MAX 5120
typedef struct logger_message {
	logger_level_t level;
	struct timespec ts;
	pid_t pid, tid;
	const char *file, *func, *module;
	int line;
	char request_id[64], session_id[64], trace_id[64];
	char text[LOGGER_MESSAGE_MAX];
	char module_storage[128], file_storage[256], function_storage[128];
} logger_message_t;
/* Only a queue owns these fixed-size snapshots. Sync records need no retained
 * source lifetime. This bounded copy adds no per-record heap allocation. */
static inline void logger_source_copy(char *dst, size_t cap, const char *src)
{
	size_t n = 0;
	while (n + 1 < cap && src[n])
		++n;
	memcpy(dst, src, n);
	dst[n] = '\0';
	if (src[n] && n)
		dst[n - 1] = '~';
}
static inline void logger_record_rebase(logger_message_t *m)
{
	m->module = m->module_storage;
	m->file = m->file_storage;
	m->func = m->function_storage;
}
static inline void logger_record_own_source(logger_message_t *m)
{
	const char *file = m->file ? strrchr(m->file, '/') : NULL;
	file = file ? file + 1 : (m->file ? m->file : "?");
	logger_source_copy(m->module_storage, sizeof(m->module_storage),
			   m->module ? m->module : "app");
	logger_source_copy(m->file_storage, sizeof(m->file_storage), file);
	logger_source_copy(m->function_storage, sizeof(m->function_storage),
			   m->func ? m->func : "?");
	logger_record_rebase(m);
}
#endif
