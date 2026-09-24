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
	/* Position differences use unsigned modular arithmetic. A queue may not
     * occupy half the position space, nor overflow its allocation size. */
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
	/* Publication precedes this lock. The consumer tests the predicate and
     * starts cond_wait while holding this SAME mutex. A publisher in that
     * interval cannot signal until cond_wait atomically releases the mutex.
     * Correctness first: do not replace this with an unlocked signal or an
     * unproven "sleeping" flag optimization. */
	pthread_mutex_lock(&q->wait_mu);
	pthread_cond_signal(&q->wait_cv);
	pthread_mutex_unlock(&q->wait_mu);
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
	/* This position measures slots taken, NOT backend completion. */
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
	/* An approximate, race-free snapshot; reservations may be unpublished.
     * Read dequeue first and clamp because two counters are not a transaction. */
	size_t deq =
		atomic_load_explicit(&q->dequeue_pos, memory_order_acquire);
	size_t enq =
		atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
	size_t depth = enq - deq;
	return depth < q->cap ? depth : q->cap;
}
