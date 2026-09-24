#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h" /* private completion-only wait: /dev/null has no fsync contract */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	logger_t *logger;
	uint64_t count;
	const char *message;
} argument_t;

static void *produce(void *p)
{
	const argument_t *a = p;
	for (uint64_t i = 0; i < a->count; ++i)
		logger_log(a->logger, LOGGER_INFO, "bench", __FILE__, __LINE__,
			   __func__, "%s", a->message);
	return NULL;
}
static double elapsed(struct timespec start, struct timespec end)
{
	return (double)(end.tv_sec - start.tv_sec) +
	       (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}
static int number(const char *s, uint64_t *out)
{
	if (!s || !*s || *s == '-')
		return -1;
	char *end;
	errno = 0;
	unsigned long long n = strtoull(s, &end, 10);
	if (errno || *end)
		return -1;
	*out = (uint64_t)n;
	return 0;
}
int main(int argc, char **argv)
{
	uint64_t nt, nr, sz;
	if (argc != 4 || number(argv[1], &nt) || number(argv[2], &nr) ||
	    number(argv[3], &sz) || nt < 1 || nt > 256 || nr < 1 || sz < 1 ||
	    sz > 4000 || nr > UINT64_MAX / nt) {
		fprintf(stderr,
			"usage: %s threads[1..256] records_per_thread message_bytes[1..4000]\n",
			argv[0]);
		return 2;
	}
	char *msg = malloc((size_t)sz + 1);
	pthread_t *threads = calloc((size_t)nt, sizeof(*threads));
	if (!msg || !threads) {
		free(msg);
		free(threads);
		return 3;
	}
	memset(msg, 'x', (size_t)sz);
	msg[sz] = '\0';
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "/dev/null";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 65536;
	c.flush_level = LOGGER_OFF;
	logger_t *l = logger_create(&c);
	if (!l) {
		perror("logger_create");
		free(msg);
		free(threads);
		return 4;
	}
	argument_t a = { l, nr, msg };
	struct timespec begin, producer_end, output_end;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	uint64_t started = 0;
	int error = 0;
	for (; started < nt; ++started) {
		error = pthread_create(&threads[started], NULL, produce, &a);
		if (error)
			break;
	}
	for (uint64_t i = 0; i < started; ++i)
		pthread_join(threads[i], NULL);
	clock_gettime(CLOCK_MONOTONIC, &producer_end);
	/* No polling of dequeue counters, no double-counted fallback, no fsync on
     * /dev/null. This waits for actual backend attempts to finish. */
	int rc = logger_wait_for_output(l);
	clock_gettime(CLOCK_MONOTONIC, &output_end);
	logger_metrics_t m;
	logger_io_metrics_t io;
	logger_get_metrics(l, &m);
	logger_get_io_metrics(l, &io);
	uint64_t drops = 0;
	for (unsigned i = 0; i < 6; ++i)
		drops += m.dropped[i];
	uint64_t attempted = started * nr;
	int valid = !error && rc == 0 &&
		    m.enqueued + m.sync_fallbacks + drops == attempted &&
		    io.async_completed == m.enqueued &&
		    io.sync_completed == m.sync_fallbacks &&
		    io.emitted_records + io.failed_records ==
			    io.async_completed + io.sync_completed;
	double ps = elapsed(begin, producer_end),
	       es = elapsed(begin, output_end);
	printf("{\"threads\":%" PRIu64 ",\"message_bytes\":%" PRIu64
	       ",\"attempted\":%" PRIu64
	       ",\"producer_seconds\":%.9f,\"end_to_end_seconds\":%.9f"
	       ",\"producer_logs_per_sec\":%.3f,\"end_to_end_logs_per_sec\":%.3f"
	       ",\"enqueued\":%" PRIu64 ",\"dropped\":%" PRIu64
	       ",\"sync_fallbacks\":%" PRIu64
	       ",\"queue_high_watermark\":%" PRIu64
	       ",\"consumer_records\":%" PRIu64 ",\"consumer_batches\":%" PRIu64
	       ",\"async_completed\":%" PRIu64 ",\"emitted_records\":%" PRIu64
	       ",\"failed_records\":%" PRIu64
	       ",\"queue_slot_bytes\":%zu,\"queue_storage_bytes\":%zu"
	       ",\"measurement\":\"output_completion_not_durability\",\"valid\":%s}\n",
	       nt, sz, attempted, ps, es, ps > 0 ? attempted / ps : 0,
	       es > 0 ? io.emitted_records / es : 0, m.enqueued, drops,
	       m.sync_fallbacks, m.queue_high_watermark, m.consumer_records,
	       m.consumer_batches, io.async_completed, io.emitted_records,
	       io.failed_records, sizeof(logger_queue_slot_t),
	       l->q.cap * sizeof(logger_queue_slot_t),
	       valid ? "true" : "false");
	logger_destroy(l);
	free(threads);
	free(msg);
	return valid ? 0 : 5;
}
