#ifndef LOGGER_QUEUE_H
#define LOGGER_QUEUE_H
#include "logger_record.h"
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define LOGGER_QUEUE_INLINE_TEXT 512u
#define LOGGER_QUEUE_SPILL_LIMIT 1024u
#define LOGGER_QUEUE_SPILL_NONE UINT32_MAX
#define LOGGER_QUEUE_SPILL_WORD_BITS 32u
#define LOGGER_QUEUE_SPILL_WORDS 	((LOGGER_QUEUE_SPILL_LIMIT + LOGGER_QUEUE_SPILL_WORD_BITS - 1u) / 	 LOGGER_QUEUE_SPILL_WORD_BITS)

_Static_assert(LOGGER_MESSAGE_MAX - 1u <= UINT16_MAX,
	       "queue text_len must fit uint16_t");
_Static_assert(sizeof(unsigned) * CHAR_BIT == LOGGER_QUEUE_SPILL_WORD_BITS,
	       "spill bitmap requires 32-bit unsigned");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
	       "spill bitmap requires lock-free unsigned atomics");

/*
 * queue slot 不内嵌完整 logger_message_t。
 * source/context 使用固定 snapshot；正文 <= 512B 直接 inline，
 * 更长正文保存 spill index，由预分配 spill pool 持有。
 */
typedef struct {
	logger_level_t level;
	struct timespec ts;
	pid_t pid, tid;
	int line;
	char request_id[64], session_id[64], trace_id[64];
	char module_storage[128], file_storage[256], function_storage[128];
	uint32_t spill_index;
	uint16_t text_len;
	uint8_t module_len, file_len, function_len;
	char inline_text[LOGGER_QUEUE_INLINE_TEXT + 1u];
} logger_queue_record_t;

typedef struct {
	_Atomic size_t seq;
	logger_queue_record_t record;
} logger_queue_slot_t;

typedef struct {
	char text[LOGGER_MESSAGE_MAX];
} logger_queue_spill_t;

typedef struct {
	/* logger_queue_init() 成功后为 OWNED；由 logger_queue_destroy() 最终释放。 */
	logger_queue_slot_t *slots;
	size_t cap, mask;

	/*
	 * long-message spill pool：queue 结构性拥有全部 block。
	 * spill_used 是 lock-free bitmap；producer 通过 0->1 CAS 独占 block，
	 * consumer 完成复制后通过 atomic clear 归还。
	 */
	logger_queue_spill_t *spills;
	size_t spill_cap;
	_Atomic unsigned spill_used[LOGGER_QUEUE_SPILL_WORDS];
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
