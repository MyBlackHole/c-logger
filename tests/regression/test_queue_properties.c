#define _POSIX_C_SOURCE 200809L
#include "logger_queue.h"
#include "support.h"
#include <stdint.h>
#include <string.h>

/* 单生产者/单消费者的确定性顺序模型。已有 queue_mpsc_regression
 * 负责八生产者并发交错，此处测试操作序列变化、背压和 spill 归还。
 */
#define STEPS 2500u
#define MAX_CAP 16u

struct expected_record {
	int line;
	size_t length;
	char text[901];
};
static struct expected_record expected[MAX_CAP];

static uint32_t next_random(uint32_t *state)
{
	uint32_t n = *state;
	n ^= n << 13;
	n ^= n >> 17;
	n ^= n << 5;
	return *state = n;
}

static size_t text_length(unsigned choice)
{
	switch (choice % 7u) {
	case 0:
		return 0;
	case 1:
		return 1;
	case 2:
		return LOGGER_QUEUE_INLINE_TEXT - 1u;
	case 3:
		return LOGGER_QUEUE_INLINE_TEXT;
	case 4:
		return LOGGER_QUEUE_INLINE_TEXT + 1u;
	case 5:
		return 700;
	default:
		return 900;
	}
}

static void write_record(logger_message_t *m, uint32_t *seed, int serial)
{
	size_t n = text_length(next_random(seed));
	memset(m, 0, sizeof(*m));
	m->level = LOGGER_INFO;
	m->line = serial;
	m->metadata_mask = LOGGER_RECORD_META_MODULE |
			   LOGGER_RECORD_META_SOURCE;
	m->module = "model";
	m->file = "prefix/model.c";
	m->func = "queue_property";
	m->text_len = (uint16_t)n;
	for (size_t j = 0; j < n; ++j)
		m->text[j] = (char)('a' + next_random(seed) % 26u);
	m->text[n] = '\0';
}

static void verify_pop(logger_queue_t *queue, size_t *front, size_t *count)
{
	logger_message_t actual = { 0 };
	int result = logger_queue_try_pop(queue, &actual);
	if (!*count) {
		CHECK(result == 0);
		return;
	}
	CHECK(result == 1);
	const struct expected_record *model = &expected[*front];
	CHECK(actual.line == model->line);
	CHECK(actual.text_len == model->length);
	CHECK(!memcmp(actual.text, model->text, model->length + 1u));
	CHECK(!strcmp(actual.module, "model"));
	CHECK(!strcmp(actual.file, "model.c"));
	CHECK(!strcmp(actual.func, "queue_property"));
	*front = (*front + 1u) % queue->cap;
	--*count;
}

static void run_model(uint32_t seed)
{
	logger_queue_t q;
	size_t requested = 2u << (seed % 4u);
	CHECK(!logger_queue_init(&q, requested));
	CHECK(q.cap == requested);
	size_t front = 0, count = 0;
	int serial = 0;
	uint64_t exhaustion = logger_queue_spill_exhaustions(&q);
	logger_message_t message;

	for (unsigned step = 0; step < STEPS; ++step) {
		unsigned choice = next_random(&seed);
		if ((choice & 3u) != 0) {
			write_record(&message, &seed, serial);
			int result = logger_queue_push(&q, &message);
			if (count == q.cap) {
				CHECK(result == 0);
			} else {
				CHECK(result == 1);
				struct expected_record *slot =
					&expected[(front + count) % q.cap];
				slot->line = serial;
				slot->length = message.text_len;
				memcpy(slot->text, message.text,
				       (size_t)message.text_len + 1u);
				++count;
				++serial;
			}
		} else {
			verify_pop(&q, &front, &count);
		}
		CHECK(logger_queue_depth(&q) == count);
		CHECK(logger_queue_empty(&q) == (count == 0));
		CHECK(logger_queue_spill_in_use(&q) <= count);
		CHECK(logger_queue_spill_in_use(&q) <= q.spill_cap);
		CHECK(logger_queue_spill_exhaustions(&q) >= exhaustion);
		exhaustion = logger_queue_spill_exhaustions(&q);
	}
	while (count)
		verify_pop(&q, &front, &count);
	CHECK(logger_queue_empty(&q));
	CHECK(!logger_queue_depth(&q));
	CHECK(!logger_queue_spill_in_use(&q));
	CHECK(!logger_queue_wait_count(&q));
	CHECK(!logger_queue_producer_wake_signals(&q));
	CHECK(!logger_queue_destroy(&q));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char *end = NULL;
	unsigned long value = strtoul(argv[1], &end, 10);
	CHECK(end && end != argv[1] && *end == '\0' && value <= UINT32_MAX);
	uint32_t seed = (uint32_t)value;
	if (!seed)
		seed = UINT32_C(0x9e3779b9);
	run_model(seed);
	printf("queue model property PASS seed=%lu steps=%u\n", value, STEPS);
	return 0;
}
