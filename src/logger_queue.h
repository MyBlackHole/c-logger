#ifndef LOGGER_QUEUE_H
#define LOGGER_QUEUE_H
#include "logger_record.h"
#include <pthread.h>
#include <stdatomic.h>
typedef struct {
	_Atomic size_t seq;
	logger_message_t msg;
} logger_queue_slot_t;
typedef struct {
	/* logger_queue_init() 成功后为 OWNED；由 logger_queue_destroy() 最终释放。 */
	logger_queue_slot_t *slots;
	size_t cap, mask;
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
#endif
