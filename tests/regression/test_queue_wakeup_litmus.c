#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include "logger_queue.h"
#include "support.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define ROUNDS 20000

static logger_queue_t queue;
static pthread_barrier_t start_barrier;
static pthread_barrier_t done_barrier;
static _Atomic unsigned worker_saw_record;
static _Atomic unsigned producer_saw_waiting;

static void wait_barrier(pthread_barrier_t *barrier)
{
	int rc = pthread_barrier_wait(barrier);

	CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *worker_side(void *arg)
{
	(void)arg;

	for (unsigned i = 0; i < ROUNDS; ++i) {
		wait_barrier(&start_barrier);
		logger_queue_wait_arm_recheck(&queue);
		unsigned seen = atomic_load_explicit(&queue.slots[0].seq,
						     memory_order_acquire) == 1;
		atomic_store_explicit(&worker_saw_record, seen,
				      memory_order_relaxed);
		wait_barrier(&done_barrier);
	}
	return NULL;
}

static void *producer_side(void *arg)
{
	(void)arg;

	for (unsigned i = 0; i < ROUNDS; ++i) {
		wait_barrier(&start_barrier);
		atomic_store_explicit(&queue.slots[0].seq, 1,
				      memory_order_release);
		unsigned seen =
			(unsigned)logger_queue_wait_probe_after_publish(&queue);
		atomic_store_explicit(&producer_saw_waiting, seen,
				      memory_order_relaxed);
		wait_barrier(&done_barrier);
	}
	return NULL;
}

static void check_reservation_gap(void)
{
	logger_queue_t q;

	CHECK(logger_queue_init(&q, 2) == 0);

	/* 位置 1 在当前出队头部（位置 0）之前发布。
	 * 它的探测可以先于工作线程布置等待状态，而不使 queue_empty 为假。
	 */
	atomic_store_explicit(&q.slots[1].seq, 2, memory_order_release);
	CHECK(!logger_queue_wait_probe_after_publish(&q));

	logger_queue_wait_arm_recheck(&q);
	CHECK(logger_queue_empty(&q));

	/* 填补头部空洞的生产者随后发布，必须看到已布置的等待者，
	 * 因此由它负责通知。
	 */
	atomic_store_explicit(&q.slots[0].seq, 1, memory_order_release);
	CHECK(logger_queue_wait_probe_after_publish(&q));

	logger_queue_wait_disarm(&q);
	CHECK(logger_queue_destroy(&q) == 0);
}

int main(void)
{
	CHECK(logger_process_init() == 0);
	check_reservation_gap();
	CHECK(logger_queue_init(&queue, 2) == 0);
	CHECK(pthread_barrier_init(&start_barrier, NULL, 3) == 0);
	CHECK(pthread_barrier_init(&done_barrier, NULL, 3) == 0);

	pthread_t worker, producer;
	CHECK(pthread_create(&worker, NULL, worker_side, NULL) == 0);
	CHECK(pthread_create(&producer, NULL, producer_side, NULL) == 0);

	unsigned worker_wins = 0;
	unsigned producer_wins = 0;
	for (unsigned i = 0; i < ROUNDS; ++i) {
		/* 主线程重置下一次判别测试实例时，两个参与者都在 done_barrier 处静止。
		 * 生产初始化后的写入只使用 QUEUE_WAKEUP_PROOF.md 说明的读改写辅助函数。
		 */
		atomic_store_explicit(&queue.consumer_waiting, 0,
				      memory_order_relaxed);
		atomic_store_explicit(&queue.slots[0].seq, 0,
				      memory_order_relaxed);
		atomic_store_explicit(&worker_saw_record, 0,
				      memory_order_relaxed);
		atomic_store_explicit(&producer_saw_waiting, 0,
				      memory_order_relaxed);

		wait_barrier(&start_barrier);
		wait_barrier(&done_barrier);

		unsigned saw_record = atomic_load_explicit(
			&worker_saw_record, memory_order_relaxed);
		unsigned saw_waiting = atomic_load_explicit(
			&producer_saw_waiting, memory_order_relaxed);

		/* 禁止结果：
		 *   工作线程未观察到以 release 发布的记录
		 *   且生产者读改写操作未观察到工作线程布置等待状态。
		 */
		CHECK(saw_record || saw_waiting);
		worker_wins += saw_record != 0;
		producer_wins += saw_waiting != 0;
	}

	CHECK(pthread_join(worker, NULL) == 0);
	CHECK(pthread_join(producer, NULL) == 0);
	CHECK(pthread_barrier_destroy(&done_barrier) == 0);
	CHECK(pthread_barrier_destroy(&start_barrier) == 0);
	logger_queue_wait_disarm(&queue);
	CHECK(logger_queue_destroy(&queue) == 0);

	printf("queue wakeup litmus: rounds=%u worker_seen=%u producer_seen=%u\n",
	       ROUNDS, worker_wins, producer_wins);
	return 0;
}
