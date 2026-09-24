#ifndef LOGGER_QUEUE_H
#define LOGGER_QUEUE_H
#include "logger_record.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define LOGGER_QUEUE_INLINE_TEXT 256u
#define LOGGER_QUEUE_SPILL_LIMIT 1024u
#define LOGGER_QUEUE_SPILL_NONE UINT32_MAX

_Static_assert(LOGGER_MESSAGE_MAX - 1u <= UINT16_MAX,
	       "queue text_len must fit uint16_t");

/*
 * queue slot 不再内嵌完整 logger_message_t。
 * source/context 仍固定 snapshot，短消息直接放 inline_text；
 * 长消息只保存 spill index，由预分配 spill pool 持有正文。
 */
typedef struct {
	logger_level_t level;
	struct timespec ts;
	pid_t pid, tid;
	int line;
	char request_id[64], session_id[64], trace_id[64];
	char module_storage[128], file_storage[256], function_storage[128];
	uint16_t text_len;
	uint32_t spill_index;
	char inline_text[LOGGER_QUEUE_INLINE_TEXT];
} logger_queue_record_t;

typedef struct {
	_Atomic size_t seq;
	logger_queue_record_t record;
} logger_queue_slot_t;

typedef struct {
	uint32_t next;
	char text[LOGGER_MESSAGE_MAX];
} logger_queue_spill_t;

typedef struct {
	/* logger_queue_init() 成功后为 OWNED；由 logger_queue_destroy() 最终释放。 */
	logger_queue_slot_t *slots;
	size_t cap, mask;

	/*
	 * 长消息 spill pool：queue 结构性拥有全部 block。
	 * spill_mu 只保护 freelist，不与 wait_mu/emit_mu/progress_mu 嵌套。
	 */
	logger_queue_spill_t *spills;
	size_t spill_cap;
	uint32_t spill_free_head;
	pthread_mutex_t spill_mu;
	_Atomic uint64_t spill_exhaustions;
	size_t storage_bytes;

	/* reservation/consumption 计数；slot.seq 负责 publish payload 可见性与复用代次。 */
	_Atomic size_t enqueue_pos;
	_Atomic size_t dequeue_pos;

	/* 仅用于 sleep/wakeup 协议，不负责串行化 slot payload 访问。 */
	pthread_mutex_t wait_mu;
	pthread_cond_t wait_cv;
} logger_queue_t;

int logger_queue_init(logger_queue_t *, size_t);
void logger_queue_destroy(logger_queue_t *);
int logger_queue_push(logger_queue_t *, const logger_message_t *);
int logger_queue_try_pop(logger_queue_t *, logger_message_t *);
size_t logger_queue_drain(logger_queue_t *, logger_message_t *, size_t);
int logger_queue_empty(logger_queue_t *);
void logger_queue_notify(logger_queue_t *);
size_t logger_queue_depth(const logger_queue_t *);

/* private benchmark/diagnostic helpers，不进入 public ABI。 */
size_t logger_queue_storage_bytes(const logger_queue_t *);
size_t logger_queue_spill_capacity(const logger_queue_t *);
size_t logger_queue_spill_storage_bytes(const logger_queue_t *);
uint64_t logger_queue_spill_exhaustions(const logger_queue_t *);

#endif
