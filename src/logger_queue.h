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

/* 队列槽位不内嵌完整 logger_message_t。
 * 来源和上下文使用固定快照；正文 <= 512B 直接内嵌，
 * 更长正文保存溢出区索引，由预分配溢出池持有。
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

	/* 长消息溢出池：队列在结构上拥有全部块。
	 * spill_used 是无锁位图；生产者通过 0->1 CAS 独占块，
	 * 消费者完成复制后通过原子清位归还。
	 */
	logger_queue_spill_t *spills;
	size_t spill_cap;
	_Atomic unsigned spill_used[LOGGER_QUEUE_SPILL_WORDS];
	_Atomic uint64_t spill_exhaustions;
	size_t storage_bytes;

	/* 预留与消费计数；slot.seq 负责已发布负载的可见性与复用代次。 */
	_Atomic size_t enqueue_pos;
	_Atomic size_t dequeue_pos;

	/* 工作线程睡眠与唤醒协议。
	 * consumer_waiting=1 表示工作线程已进入 wait_mu 保护的“准备睡眠/已睡眠”状态；
	 * 生产者只有观察到 1 才进入 wait_mu + cond_signal 慢路径。
	 */
	_Atomic unsigned consumer_waiting;
	_Atomic uint64_t wait_count;
	_Atomic uint64_t producer_wake_signals;
	_Atomic uint64_t force_wake_signals;
	/* wait_mu 解锁失败后，无法证明工作线程已能继续运行或安全销毁。 */
	_Atomic int wait_mu_error;
	pthread_mutex_t wait_mu;
	pthread_cond_t wait_cv;
} logger_queue_t;

/* 防止单消费者丢失唤醒的交接协议。
 *
 * 初始化后，对 consumer_waiting 的所有并发写入都是 acq_rel 读改写操作。
 * 这是有意为之：生产者只在 slot.seq 发布后探测，
 * 读改写操作的修改顺序链会将该发布传递给工作线程后续的等待状态布置。
 * 不要用普通读写或栅栏替换这些辅助函数。
 *
 * 参见 docs/v2/QUEUE_WAKEUP_PROOF.md。
 */
static inline void logger_queue_wait_arm_recheck(logger_queue_t *q)
{
	(void)atomic_exchange_explicit(&q->consumer_waiting, 1,
				       memory_order_acq_rel);
}

static inline int
logger_queue_wait_probe_after_publish(logger_queue_t *q)
{
	return atomic_fetch_or_explicit(&q->consumer_waiting, 0,
					memory_order_acq_rel) != 0;
}

static inline int logger_queue_wait_claim_signal(logger_queue_t *q)
{
	return atomic_exchange_explicit(&q->consumer_waiting, 0,
					memory_order_acq_rel) != 0;
}

static inline void logger_queue_wait_disarm(logger_queue_t *q)
{
	(void)atomic_exchange_explicit(&q->consumer_waiting, 0,
				       memory_order_acq_rel);
}

int logger_queue_init(logger_queue_t *, size_t);
int logger_queue_destroy(logger_queue_t *);
/* 返回 1 表示入队成功，0 表示未入队，负值表示已发布但唤醒失败。 */
int logger_queue_push(logger_queue_t *, const logger_message_t *);
int logger_queue_try_pop(logger_queue_t *, logger_message_t *);
size_t logger_queue_drain(logger_queue_t *, logger_message_t *, size_t);
int logger_queue_empty(logger_queue_t *);
/* 通知和强制唤醒均返回 0 或负 errno；负值不能被当作入队失败回滚。 */
int logger_queue_notify_if_waiting(logger_queue_t *);
int logger_queue_wake_force(logger_queue_t *);
int logger_queue_wait_error(const logger_queue_t *);
size_t logger_queue_depth(const logger_queue_t *);

/* 私有基准测试与诊断辅助函数，不进入公开 ABI。 */
size_t logger_queue_storage_bytes(const logger_queue_t *);
size_t logger_queue_spill_capacity(const logger_queue_t *);
size_t logger_queue_spill_storage_bytes(const logger_queue_t *);
uint64_t logger_queue_spill_exhaustions(const logger_queue_t *);
size_t logger_queue_spill_in_use(const logger_queue_t *);
uint64_t logger_queue_wait_count(const logger_queue_t *);
uint64_t logger_queue_producer_wake_signals(const logger_queue_t *);
uint64_t logger_queue_force_wake_signals(const logger_queue_t *);

#endif
