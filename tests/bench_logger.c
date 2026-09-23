#define LOG_MODULE "bench"
#include "logger.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
	long id;
	long count;
} arg_t;
static void *run(void *p)
{
	arg_t *a = (arg_t *)p;
	for (long i = 0; i < a->count; i++)
		LOG_INFO("worker=%ld seq=%ld value=%d", a->id, i, 42);
	return NULL;
}
static double sec(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 8;
	long each = argc > 2 ? atol(argv[2]) : 100000;
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./bench.log";
	c.async_mode = 1;
	c.queue_capacity = 65536;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1024ull * 1024ull * 1024ull;
	c.rotation.retention_days = 1;
	c.flush_level = LOGGER_FATAL;
	if (logger_init(&c)) {
		perror("logger_init");
		return 1;
	}
	pthread_t *ts = calloc((size_t)n, sizeof(*ts));
	arg_t *as = calloc((size_t)n, sizeof(*as));
	double a = sec();
	for (int i = 0; i < n; i++) {
		as[i] = (arg_t){ i, each };
		pthread_create(&ts[i], NULL, run, &as[i]);
	}
	for (int i = 0; i < n; i++)
		pthread_join(ts[i], NULL);
	double b = sec();
	logger_metrics_t m;
	logger_get_global_metrics(&m);
	unsigned long long dropped =
		(unsigned long long)logger_global_dropped();
	logger_shutdown();
	printf("threads=%d attempted=%ld producer_seconds=%.6f attempted_logs_per_sec=%.0f dropped=%llu hwm=%llu enqueued=%llu batches=%llu records=%llu sync_fb=%llu\n",
	       n, each * n, b - a, (each * n) / (b - a), dropped,
	       (unsigned long long)m.queue_high_watermark,
	       (unsigned long long)m.enqueued,
	       (unsigned long long)m.consumer_batches,
	       (unsigned long long)m.consumer_records,
	       (unsigned long long)m.sync_fallbacks);
	free(ts);
	free(as);
	return 0;
}
