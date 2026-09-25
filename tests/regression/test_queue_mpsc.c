#define _POSIX_C_SOURCE 200809L
#include "logger_queue.h"
#include "support.h"
#include <stdint.h>
#include <sched.h>
#include <string.h>

#define PRODUCERS 8
#define RECORDS 3000
static logger_queue_t queue;
static _Atomic int running = 1, producers_done;
static int last[PRODUCERS];
static unsigned consumed;
static void *produce(void *arg)
{
	int id = *(int *)arg;
	logger_message_t m = { 0 };
	m.pid = id;
	/* 让并发 MPSC 回归真实覆盖 long-message bitmap allocator。 */
	memset(m.text, 'P', 700);
	m.text[700] = '\0';
	m.text_len = 700;
	for (int i = 0; i < RECORDS; ++i) {
		m.line = i;
		while (!logger_queue_push(&queue, &m))
			sched_yield();
	}
	return NULL;
}
static void *observe(void *arg)
{
	(void)arg;
	while (!atomic_load(&producers_done)) {
		CHECK(logger_queue_depth(&queue) <= queue.cap);
		sched_yield();
	}
	return NULL;
}
static void *consume(void *arg)
{
	(void)arg;
	logger_message_t m;
	for (;;) {
		if (logger_queue_try_pop(&queue, &m)) {
			CHECK(m.pid >= 0 && m.pid < PRODUCERS);
			CHECK(m.line ==
			      last[m.pid] +
				      1); /* no duplication, loss, or reordering per producer */
			last[m.pid] = m.line;
			++consumed;
			continue;
		}
		pthread_mutex_lock(&queue.wait_mu);
		while (logger_queue_empty(&queue) && atomic_load(&running)) {
			atomic_store_explicit(&queue.consumer_waiting, 1,
					      memory_order_release);
			atomic_thread_fence(memory_order_seq_cst);
			if (!logger_queue_empty(&queue) || !atomic_load(&running))
				break;
			atomic_fetch_add_explicit(&queue.wait_count, 1,
						  memory_order_relaxed);
			pthread_cond_wait(&queue.wait_cv, &queue.wait_mu);
		}
		atomic_store_explicit(&queue.consumer_waiting, 0,
				      memory_order_release);
		int stop = logger_queue_empty(&queue) && !atomic_load(&running);
		pthread_mutex_unlock(&queue.wait_mu);
		if (stop)
			return NULL;
	}
}
static void check_compact_storage(void)
{
	CHECK(sizeof(logger_queue_slot_t) < sizeof(logger_message_t));

	logger_queue_t q;
	CHECK(logger_queue_init(&q, LOGGER_QUEUE_SPILL_LIMIT * 2u) == 0);
	CHECK(q.spill_cap == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(logger_queue_storage_bytes(&q) ==
	      q.cap * sizeof(logger_queue_slot_t) +
		      q.spill_cap * sizeof(logger_queue_spill_t));
	CHECK(logger_queue_storage_bytes(&q) <
	      q.cap * sizeof(logger_message_t));

	logger_message_t in = { .level = LOGGER_INFO,
				.module = "compact",
				.file = "dir/compact.c",
				.func = "push" },
			 out;
	memset(in.text, 'L', 700);
	in.text[700] = '\0';

	for (size_t i = 0; i < q.spill_cap; ++i) {
		in.line = (int)i;
		CHECK(logger_queue_push(&q, &in) == 1);
	}
	CHECK(logger_queue_depth(&q) == q.spill_cap);
	CHECK(logger_queue_depth(&q) < q.cap);
	/* 没有 consumer 准备睡眠时，producer 不应该执行任何 cond signal。 */
	CHECK(logger_queue_producer_wake_signals(&q) == 0);

	in.line = 9999;
	CHECK(logger_queue_push(&q, &in) == 0);
	CHECK(logger_queue_spill_exhaustions(&q) == 1);

	CHECK(logger_queue_try_pop(&q, &out) == 1);
	CHECK(out.line == 0);
	CHECK(strlen(out.text) == 700);
	CHECK(!strcmp(out.module, "compact"));
	CHECK(!strcmp(out.file, "compact.c"));
	CHECK(!strcmp(out.func, "push"));

	CHECK(logger_queue_push(&q, &in) == 1);
	size_t drained = 0;
	while (logger_queue_try_pop(&q, &out)) {
		CHECK(strlen(out.text) == 700);
		++drained;
	}
	CHECK(drained == q.spill_cap);
	logger_queue_destroy(&q);

	CHECK(logger_queue_init(&q, 2) == 0);
	memset(&in, 0, sizeof(in));
	memset(in.text, 'S', LOGGER_QUEUE_INLINE_TEXT);
	in.text[LOGGER_QUEUE_INLINE_TEXT] = '\0';
	in.text_len = LOGGER_QUEUE_INLINE_TEXT;
	CHECK(logger_queue_push(&q, &in) == 1);
	CHECK(logger_queue_spill_exhaustions(&q) == 0);
	CHECK(logger_queue_try_pop(&q, &out) == 1);
	CHECK(out.text_len == LOGGER_QUEUE_INLINE_TEXT);
	CHECK(strlen(out.text) == LOGGER_QUEUE_INLINE_TEXT);
	CHECK(!memcmp(out.text, in.text, LOGGER_QUEUE_INLINE_TEXT + 1u));
	logger_queue_destroy(&q);
}

static void check_wrap(void)
{
	logger_queue_t q;
	CHECK(logger_queue_init(&q, 8) == 0);
	size_t start = SIZE_MAX - 3;
	for (size_t i = 0; i < q.cap; ++i)
		atomic_store(&q.slots[(start + i) & q.mask].seq, start + i);
	atomic_store(&q.enqueue_pos, start);
	atomic_store(&q.dequeue_pos, start);
	logger_message_t in = { 0 }, out;
	for (int pass = 0; pass < 3; ++pass) {
		for (int i = 0; i < 8; ++i) {
			in.line = pass * 8 + i;
			CHECK(logger_queue_push(&q, &in));
		}
		CHECK(!logger_queue_push(&q, &in));
		for (int i = 0; i < 8; ++i) {
			CHECK(logger_queue_try_pop(&q, &out));
			CHECK(out.line == pass * 8 + i);
		}
		CHECK(logger_queue_empty(&q));
	}
	logger_queue_destroy(&q);
}
int main(void)
{
	check_compact_storage();
	check_wrap();
	CHECK(logger_queue_init(&queue, 64) == 0);
	pthread_t producers[PRODUCERS], consumer, observer;
	int ids[PRODUCERS];
	for (int i = 0; i < PRODUCERS; ++i)
		last[i] = -1;
	CHECK(pthread_create(&consumer, NULL, consume, NULL) == 0);
	CHECK(pthread_create(&observer, NULL, observe, NULL) == 0);
	for (int i = 0; i < PRODUCERS; ++i) {
		ids[i] = i;
		CHECK(pthread_create(&producers[i], NULL, produce, &ids[i]) ==
		      0);
	}
	for (int i = 0; i < PRODUCERS; ++i)
		CHECK(pthread_join(producers[i], NULL) == 0);
	atomic_store(&producers_done, 1);
	CHECK(pthread_join(observer, NULL) == 0);
	atomic_store(&running, 0);
	logger_queue_wake_force(&queue);
	CHECK(pthread_join(consumer, NULL) == 0);
	CHECK(consumed == PRODUCERS * RECORDS);
	for (int i = 0; i < PRODUCERS; ++i)
		CHECK(last[i] == RECORDS - 1);
	logger_queue_destroy(&queue);
	CHECK(logger_queue_init(&queue, SIZE_MAX) == -1 && errno == EOVERFLOW);
	puts("512B inline + lock-free spill bitmap; 8 long-message producers exact; depth + rollover checked");
	return 0;
}
