#ifndef LOGGER_RECORD_H
#define LOGGER_RECORD_H
#include "logger.h"
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#define LOGGER_MESSAGE_MAX 4096
#define LOGGER_LINE_MAX 5120

enum {
	LOGGER_RECORD_META_MODULE = 1u << 0,
	LOGGER_RECORD_META_CONTEXT = 1u << 1,
	LOGGER_RECORD_META_SOURCE = 1u << 2
};

typedef struct logger_message {
	logger_level_t level;
	struct timespec ts;
	pid_t pid, tid;
	const char *file, *func, *module;
	int line;
	/* internal-only：告诉 async queue 哪些 metadata snapshot 真正有语义。 */
	uint8_t metadata_mask;
	char request_id[64], session_id[64], trace_id[64];
	/* 记录实际存入 text[] 的字节数，不含 NUL；避免 async queue 再扫描正文。 */
	uint16_t text_len;
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
static inline size_t logger_record_text_length(const logger_message_t *m)
{
	size_t n = m->text_len;
	/*
	 * production 路径由 vsnprintf() 精确填充 text_len。
	 * 单元测试和内部手工构造 record 允许 text_len=0，此时兼容性扫描一次。
	 */
	if ((n != 0 || m->text[0] == '\0') && n < LOGGER_MESSAGE_MAX &&
	    m->text[n] == '\0')
		return n;
	n = 0;
	while (n < LOGGER_MESSAGE_MAX && m->text[n])
		++n;
	return n;
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
