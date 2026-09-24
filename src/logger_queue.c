#include "logger_queue.h"
#include "logger_cleanup.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int queue_capacity(size_t requested, size_t *capacity)
{
	size_t n = 2;
	if (requested < 2)
		requested = 2;
	/* position 差值使用 unsigned 模运算。queue 不能占用一半及以上的 position
	 * 空间，同时必须避免 allocation size 溢出。 */
	while (n < requested) {
		if (n > SIZE_MAX / 2) {
			errno = EOVERFLOW;
			return -1;
		}
		n *= 2;
	}
	if (n > SIZE_MAX / 2 || n > SIZE_MAX / sizeof(logger_queue_slot_t)) {
		errno = EOVERFLOW;
		return -1;
	}
	*capacity = n;
	return 0;
}

static size_t queue_spill_capacity(size_t queue_cap)
{
	return queue_cap < LOGGER_QUEUE_SPILL_LIMIT ?
		       queue_cap :
		       LOGGER_QUEUE_SPILL_LIMIT;
}

static int queue_storage_size(size_t queue_cap, size_t spill_cap,
			      size_t *total)
{
	if (spill_cap > SIZE_MAX / sizeof(logger_queue_spill_t))
		return -EOVERFLOW;
	size_t spill_bytes = spill_cap * sizeof(logger_queue_spill_t);
	if (queue_cap > (SIZE_MAX - spill_bytes) / sizeof(logger_queue_slot_t))
		return -EOVERFLOW;
	*total = queue_cap * sizeof(logger_queue_slot_t) + spill_bytes;
	return 0;
}

static void queue_copy_source(logger_queue_record_t *dst,
			      const logger_message_t *src)
{
	const char *file = src->file ? strrchr(src->file, '/') : NULL;
	file = file ? file + 1 : (src->file ? src->file : "?");

	/*
	 * slot 会反复复用。先清零固定 source storage，避免短字符串把上一条
	 * record 的尾部字节带进后续 worker batch。
	 */
	memset(dst->module_storage, 0, sizeof(dst->module_storage));
	memset(dst->file_storage, 0, sizeof(dst->file_storage));
	memset(dst->function_storage, 0, sizeof(dst->function_storage));
	logger_source_copy(dst->module_storage, sizeof(dst->module_storage),
			   src->module ? src->module : "app");
	logger_source_copy(dst->file_storage, sizeof(dst->file_storage), file);
	logger_source_copy(dst->function_storage, sizeof(dst->function_storage),
			   src->func ? src->func : "?");
}

static void queue_record_pack(logger_queue_record_t *dst,
			      const logger_message_t *src, size_t text_len,
			      uint32_t spill_index)
{
	dst->level = src->level;
	dst->ts = src->ts;
	dst->pid = src->pid;
	dst->tid = src->tid;
	dst->line = src->line;
	memcpy(dst->request_id, src->request_id, sizeof(dst->request_id));
	memcpy(dst->session_id, src->session_id, sizeof(dst->session_id));
	memcpy(dst->trace_id, src->trace_id, sizeof(dst->trace_id));
	queue_copy_source(dst, src);
	dst->text_len = (uint16_t)text_len;
	dst->spill_index = spill_index;
	if (spill_index == LOGGER_QUEUE_SPILL_NONE)
		memcpy(dst->inline_text, src->text, text_len + 1);
}

static void queue_record_unpack(logger_queue_t *q,
				const logger_queue_record_t *src,
				logger_message_t *dst)
{
	dst->level = src->level;
	dst->ts = src->ts;
	dst->pid = src->pid;
	dst->tid = src->tid;
	dst->line = src->line;
	memcpy(dst->request_id, src->request_id, sizeof(dst->request_id));
	memcpy(dst->session_id, src->session_id, sizeof(dst->session_id));
	memcpy(dst->trace_id, src->trace_id, sizeof(dst->trace_id));
	memcpy(dst->module_storage, src->module_storage,
	       sizeof(dst->module_storage));
	memcpy(dst->file_storage, src->file_storage, sizeof(dst->file_storage));
	memcpy(dst->function_storage, src->function_storage,
	       sizeof(dst->function_storage));
	logger_record_rebase(dst);

	const char *text = src->spill_index == LOGGER_QUEUE_SPILL_NONE ?
				   src->inline_text :
				   q->spills[src->spill_index].text;
	memcpy(dst->text, text, (size_t)src->text_len + 1);
}

static int queue_spill_take(logger_queue_t *q, const char *text,
			    size_t text_len, uint32_t *index)
{
	uint32_t found;
	{
		guard(pthread_mutex)(&q->spill_mu);
		found = q->spill_free_head;
		if (found == LOGGER_QUEUE_SPILL_NONE) {
			atomic_fetch_add_explicit(&q->spill_exhaustions, 1,
						  memory_order_relaxed);
			return 0;
		}
		q->spill_free_head = q->spills[found].next;
		q->spills[found].next = LOGGER_QUEUE_SPILL_NONE;
	}

	/*
	 * block 已从 freelist 移除，当前 producer 独占它；
	 * memcpy 不需要持有 spill_mu，也不会阻塞其他长消息 producer。
	 */
	memcpy(q->spills[found].text, text, text_len + 1);
	*index = found;
	return 1;
}

static void queue_spill_put(logger_queue_t *q, uint32_t index)
{
	if (index == LOGGER_QUEUE_SPILL_NONE)
		return;
	guard(pthread_mutex)(&q->spill_mu);
	q->spills[index].next = q->spill_free_head;
	q->spill_free_head = index;
}

int logger_queue_init(logger_queue_t *q, size_t requested)
{
	memset(q, 0, sizeof(*q));
	if (queue_capacity(requested, &q->cap) != 0)
		return -1;
	q->mask = q->cap - 1;
	q->spill_cap = queue_spill_capacity(q->cap);
	if (queue_storage_size(q->cap, q->spill_cap, &q->storage_bytes) != 0) {
		errno = EOVERFLOW;
		return -1;
	}

	logger_queue_slot_t *slots __free(free) =
		calloc(q->cap, sizeof(*slots));
	logger_queue_spill_t *spills __free(free) =
		calloc(q->spill_cap, sizeof(*spills));
	if (!slots || !spills)
		return -1;

	for (size_t i = 0; i < q->cap; ++i)
		atomic_init(&slots[i].seq, i);
	for (size_t i = 0; i < q->spill_cap; ++i)
		spills[i].next = i + 1 < q->spill_cap ?
					 (uint32_t)(i + 1) :
					 LOGGER_QUEUE_SPILL_NONE;
	q->spill_free_head = 0;
	atomic_init(&q->spill_exhaustions, 0);
	atomic_init(&q->enqueue_pos, 0);
	atomic_init(&q->dequeue_pos, 0);

	int rc = pthread_mutex_init(&q->spill_mu, NULL);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	rc = pthread_mutex_init(&q->wait_mu, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&q->spill_mu);
		errno = rc;
		return -1;
	}
	rc = pthread_cond_init(&q->wait_cv, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&q->wait_mu);
		pthread_mutex_destroy(&q->spill_mu);
		errno = rc;
		return -1;
	}

	/* ownership transfer：两个预分配区域都交给 logger_queue_t。 */
	q->slots = no_free_ptr(slots);
	q->spills = no_free_ptr(spills);
	return 0;
}

void logger_queue_destroy(logger_queue_t *q)
{
	pthread_cond_destroy(&q->wait_cv);
	pthread_mutex_destroy(&q->wait_mu);
	pthread_mutex_destroy(&q->spill_mu);
	free(q->spills);
	q->spills = NULL;
	free(q->slots);
	q->slots = NULL;
}

void logger_queue_notify(logger_queue_t *q)
{
	/* payload publication 发生在这里加锁之前。consumer 在持有同一把 wait_mu 时
	 * 检查 predicate 并进入 pthread_cond_wait()；在这段窗口内 publisher
	 * 无法完成 signal，直到 cond_wait 原子地释放 wait_mu，因此不会丢 wakeup。
	 * 正确性优先：不能改成无锁 signal，也不要引入未经证明的 "sleeping" flag 优化。 */
	guard(pthread_mutex)(&q->wait_mu);
	pthread_cond_signal(&q->wait_cv);
}

int logger_queue_push(logger_queue_t *q, const logger_message_t *m)
{
	size_t text_len = 0;
	while (text_len < LOGGER_MESSAGE_MAX && m->text[text_len])
		++text_len;
	if (text_len == LOGGER_MESSAGE_MAX) {
		errno = EOVERFLOW;
		return 0;
	}

	uint32_t spill_index = LOGGER_QUEUE_SPILL_NONE;
	if (text_len >= LOGGER_QUEUE_INLINE_TEXT &&
	    !queue_spill_take(q, m->text, text_len, &spill_index))
		return 0;

	size_t pos =
		atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
	for (;;) {
		logger_queue_slot_t *slot = &q->slots[pos & q->mask];
		size_t seq =
			atomic_load_explicit(&slot->seq, memory_order_acquire);
		size_t diff = seq - pos;
		if (diff == 0) {
			if (atomic_compare_exchange_weak_explicit(
				    &q->enqueue_pos, &pos, pos + 1,
				    memory_order_relaxed,
				    memory_order_relaxed)) {
				queue_record_pack(&slot->record, m, text_len,
						  spill_index);
				atomic_store_explicit(&slot->seq, pos + 1,
						      memory_order_release);
				logger_queue_notify(q);
				return 1;
			}
		} else if (diff > SIZE_MAX / 2) {
			queue_spill_put(q, spill_index);
			return 0;
		} else {
			pos = atomic_load_explicit(&q->enqueue_pos,
						   memory_order_relaxed);
		}
	}
}

int logger_queue_try_pop(logger_queue_t *q, logger_message_t *m)
{
	size_t pos =
		atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
	logger_queue_slot_t *slot = &q->slots[pos & q->mask];
	if (atomic_load_explicit(&slot->seq, memory_order_acquire) != pos + 1)
		return 0;

	uint32_t spill_index = slot->record.spill_index;
	queue_record_unpack(q, &slot->record, m);
	/*
	 * consumer 已把正文复制到 worker batch，可先归还 spill block；
	 * 归还必须发生在 slot 标记可复用之前，避免 producer 覆盖 spill_index。
	 */
	queue_spill_put(q, spill_index);
	atomic_store_explicit(&slot->seq, pos + q->cap, memory_order_release);
	/* 这里只表示 slot 已被 consumer 取走，不代表 backend 已完成输出。 */
	atomic_store_explicit(&q->dequeue_pos, pos + 1, memory_order_release);
	return 1;
}

size_t logger_queue_drain(logger_queue_t *q, logger_message_t *out, size_t max)
{
	size_t n = 0;
	while (n < max && logger_queue_try_pop(q, &out[n]))
		++n;
	return n;
}

int logger_queue_empty(logger_queue_t *q)
{
	size_t pos =
		atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
	logger_queue_slot_t *slot = &q->slots[pos & q->mask];
	return atomic_load_explicit(&slot->seq, memory_order_acquire) !=
	       pos + 1;
}

size_t logger_queue_depth(const logger_queue_t *q)
{
	/* 这是 race-free 的近似快照；reservation 可能尚未 publish。
	 * 先读取 dequeue，再对深度做 clamp，因为两个 counter 不是一个原子事务。 */
	size_t deq =
		atomic_load_explicit(&q->dequeue_pos, memory_order_acquire);
	size_t enq =
		atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
	size_t depth = enq - deq;
	return depth < q->cap ? depth : q->cap;
}

size_t logger_queue_storage_bytes(const logger_queue_t *q)
{
	return q ? q->storage_bytes : 0;
}

size_t logger_queue_spill_capacity(const logger_queue_t *q)
{
	return q ? q->spill_cap : 0;
}

size_t logger_queue_spill_storage_bytes(const logger_queue_t *q)
{
	return q ? q->spill_cap * sizeof(logger_queue_spill_t) : 0;
}

uint64_t logger_queue_spill_exhaustions(const logger_queue_t *q)
{
	return q ? atomic_load_explicit(&q->spill_exhaustions,
					memory_order_relaxed) :
		   0;
}
