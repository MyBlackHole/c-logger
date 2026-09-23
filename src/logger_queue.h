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
	logger_queue_slot_t *slots;
	size_t cap, mask;
	_Atomic size_t enqueue_pos;
	_Atomic size_t dequeue_pos;
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
