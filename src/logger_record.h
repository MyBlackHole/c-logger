#ifndef LOGGER_RECORD_H
#define LOGGER_RECORD_H
#include "logger.h"
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#define LOGGER_MESSAGE_MAX 4096
#define LOGGER_LINE_MAX 5120

/* 普通日志保留长度受限的字节前缀，并添加可见标记。
 * 严格同步路径拒绝超长文本，不使用此机制；两种路径均返回状态。
 */
static inline void logger_text_mark_truncated(char *text, size_t capacity)
{
	static const char marker[] = " [truncated]";
	if (capacity >= sizeof(marker))
		memcpy(text + capacity - sizeof(marker), marker,
		       sizeof(marker));
	else if (capacity >= 2) {
		text[capacity - 2] = '~';
		text[capacity - 1] = '\0';
	}
}

enum {
	LOGGER_RECORD_META_MODULE = 1u << 0,
	LOGGER_RECORD_META_CONTEXT = 1u << 1,
	LOGGER_RECORD_META_SOURCE = 1u << 2
};

typedef struct logger_message {
	logger_level_t level;
	struct timespec ts;
	pid_t pid;
	pid_t tid;
	const char *file;
	const char *func;
	const char *module;
	int line;
	/* 仅供内部使用：告诉异步队列哪些元数据快照真正有语义。 */
	uint8_t metadata_mask;
	char request_id[64];
	char session_id[64];
	char trace_id[64];
	/* 记录实际存入 text[] 的字节数，不含 NUL；避免异步队列再扫描正文。 */
	uint16_t text_len;
	char text[LOGGER_MESSAGE_MAX];
	char module_storage[128];
	char file_storage[256];
	char function_storage[128];
} logger_message_t;
/* 只有队列拥有这些固定大小的快照。同步记录无需保留来源的生命周期。
 * 这种有界复制不会为每条记录增加堆分配。
 */
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
	/* 生产路径由 vsnprintf() 精确填充 text_len。
	 * 单元测试和内部手工构造的记录允许 text_len=0，此时兼容性扫描一次。
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
