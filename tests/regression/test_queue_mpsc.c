#define _POSIX_C_SOURCE 200809L
#include "logger_queue.h"
#include "support.h"
#include <stdint.h>
#include <sched.h>

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
		while (logger_queue_empty(&queue) && atomic_load(&running))
			pthread_cond_wait(&queue.wait_cv, &queue.wait_mu);
		int stop = logger_queue_empty(&queue) && !atomic_load(&running);
		pthread_mutex_unlock(&queue.wait_mu);
		if (stop)
			return NULL;
	}
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
	logger_queue_notify(&queue);
	CHECK(pthread_join(consumer, NULL) == 0);
	CHECK(consumed == PRODUCERS * RECORDS);
	for (int i = 0; i < PRODUCERS; ++i)
		CHECK(last[i] == RECORDS - 1);
	logger_queue_destroy(&queue);
	CHECK(logger_queue_init(&queue, SIZE_MAX) == -1 && errno == EOVERFLOW);
	puts("8 producers: 24000 exact records; concurrent depth reads; counter rollover checked");
	return 0;
}
