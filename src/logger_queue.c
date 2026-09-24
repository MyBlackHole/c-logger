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

int logger_queue_init(logger_queue_t *q, size_t requested)
{
	memset(q, 0, sizeof(*q));
	if (queue_capacity(requested, &q->cap) != 0)
		return -1;
	q->mask = q->cap - 1;

	logger_queue_slot_t *slots __free(free) =
		calloc(q->cap, sizeof(*slots));
	if (!slots)
		return -1;
	for (size_t i = 0; i < q->cap; ++i)
		atomic_init(&slots[i].seq, i);
	atomic_init(&q->enqueue_pos, 0);
	atomic_init(&q->dequeue_pos, 0);

	int rc = pthread_mutex_init(&q->wait_mu, NULL);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	rc = pthread_cond_init(&q->wait_cv, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&q->wait_mu);
		errno = rc;
		return -1;
	}

	/* ownership transfer：词法临时 owner -> logger_queue_t。
	 * 最终由 logger_queue_destroy() 释放。 */
	q->slots = no_free_ptr(slots);
	return 0;
}

void logger_queue_destroy(logger_queue_t *q)
{
	pthread_cond_destroy(&q->wait_cv);
	pthread_mutex_destroy(&q->wait_mu);
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
				slot->msg = *m;
				logger_record_own_source(&slot->msg);
				atomic_store_explicit(&slot->seq, pos + 1,
						      memory_order_release);
				logger_queue_notify(q);
				return 1;
			}
		} else if (diff > SIZE_MAX / 2) {
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
	*m = slot->msg;
	logger_record_rebase(m);
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
