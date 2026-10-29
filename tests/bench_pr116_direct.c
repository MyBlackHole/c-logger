#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/*
 * PR #116 同一份测试驱动同时链接两个历史提交各自的生产静态库。
 * 仅用公开 API；纳秒分位是调用返回耗时，不是逐条输出/落盘耗时。
 */
struct start_gate {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int go;
};

struct producer {
	logger_t *logger;
	const char *payload;
	struct start_gate *gate;
	uint64_t count;
	uint64_t *latency;
	int error;
};

static uint64_t clock_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		abort();
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)ts.tv_nsec;
}

static int number(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long v;

	if (!text || !*text || *text == '-')
		return -1;
	errno = 0;
	v = strtoull(text, &end, 10);
	if (errno || *end)
		return -1;
	*value = (uint64_t)v;
	return 0;
}

static void *produce(void *opaque)
{
	struct producer *p = opaque;
	int rc = pthread_mutex_lock(&p->gate->mutex);

	if (rc) {
		p->error = rc;
		return NULL;
	}
	while (!p->gate->go) {
		rc = pthread_cond_wait(&p->gate->cond, &p->gate->mutex);
		if (rc) {
			p->error = rc;
			pthread_mutex_unlock(&p->gate->mutex);
			return NULL;
		}
	}
	int go = p->gate->go;
	pthread_mutex_unlock(&p->gate->mutex);
	if (go < 0)
		return NULL;

	for (uint64_t i = 0; i < p->count; ++i) {
		uint64_t start = clock_ns();
		logger_log(p->logger, LOGGER_INFO, "bench", __FILE__, __LINE__,
			   __func__, "%s", p->payload);
		uint64_t diff = clock_ns() - start;
		p->latency[i] = diff ? diff : 1;
	}
	return NULL;
}

static int cmp_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t pct(const uint64_t *values, size_t n, size_t percentage)
{
	return values[(n * percentage + 99) / 100 - 1];
}

static uint64_t dropped_count(const logger_metrics_t *m)
{
	uint64_t count = 0;

	for (unsigned int i = 0; i < 6; ++i)
		count += m->dropped[i];
	return count;
}

int main(int argc, char **argv)
{
	uint64_t nt;
	uint64_t nr;
	uint64_t bytes;
	uint64_t cap;
	if (argc != 5 || number(argv[1], &nt) || number(argv[2], &nr) ||
	    number(argv[3], &bytes) || number(argv[4], &cap)) {
		fputs("usage: threads records bytes capacity\n", stderr);
		return 2;
	}
	if (nt < 1 || nt > 16 || nr < 1 || nr > 20000 || bytes < 1 ||
	    bytes > 1024 || cap < 64 || cap > 65536 || nt * nr > 200000)
		return 2;

	char *message = malloc((size_t)bytes + 1);
	uint64_t *latency = calloc((size_t)(nt * nr), sizeof(*latency));
	pthread_t *threads = calloc((size_t)nt, sizeof(*threads));
	struct producer *args = calloc((size_t)nt, sizeof(*args));

	if (!message || !latency || !threads || !args)
		return 3;
	memset(message, 'x', (size_t)bytes);
	message[bytes] = '\0';

	logger_config_t config = LOGGER_DEFAULT_CONFIG();
	config.outputs = LOGGER_OUT_FILE;
	config.file_path = "/dev/null";
	config.rotation.mode = LOGGER_ROTATE_NONE;
	config.async_mode = 1;
	config.queue_capacity = (size_t)cap;
	config.flush_level = LOGGER_OFF;
	config.overflow[LOGGER_INFO] = LOGGER_OVERFLOW_DROP;
	logger_t *logger = logger_create(&config);
	if (!logger) {
		perror("logger_create");
		return 4;
	}

	struct start_gate gate = { 0 };
	if (pthread_mutex_init(&gate.mutex, NULL))
		return 3;
	if (pthread_cond_init(&gate.cond, NULL)) {
		pthread_mutex_destroy(&gate.mutex);
		return 3;
	}
	size_t started = 0;
	for (; started < (size_t)nt; ++started) {
		args[started].logger = logger;
		args[started].payload = message;
		args[started].gate = &gate;
		args[started].count = nr;
		args[started].latency = latency + started * (size_t)nr;
		if (pthread_create(&threads[started], NULL, produce,
				   &args[started]))
			break;
	}
	pthread_mutex_lock(&gate.mutex);
	uint64_t begin = clock_ns();
	gate.go = started == (size_t)nt ? 1 : -1;
	pthread_cond_broadcast(&gate.cond);
	pthread_mutex_unlock(&gate.mutex);

	int producer_error = started != (size_t)nt;
	for (size_t i = 0; i < started; ++i) {
		if (pthread_join(threads[i], NULL))
			producer_error = 1;
		if (args[i].error)
			producer_error = 1;
	}
	uint64_t producer_end = clock_ns();
	logger_metrics_t metrics;
	logger_get_metrics(logger, &metrics);

	/* 出队计数不等于后端已经完成；仅等待公开输出完成计数。 */
	logger_io_metrics_t io = { 0 };
	uint64_t deadline = clock_ns() + UINT64_C(10000000000);
	int drained = 0;
	while (clock_ns() < deadline) {
		logger_get_io_metrics(logger, &io);
		if (io.async_completed == metrics.enqueued &&
		    io.sync_completed == metrics.sync_fallbacks) {
			drained = 1;
			break;
		}
		struct timespec sleep_time = { 0, 1000000L };
		nanosleep(&sleep_time, NULL);
	}
	uint64_t output_end = clock_ns();
	logger_diagnostics_t diag;
	logger_get_diagnostics(logger, &diag);

	size_t count = (size_t)(nt * nr);
	uint64_t drops = dropped_count(&metrics);
	int valid = !producer_error && drained && !io.first_error &&
		    !io.failed_records &&
		    metrics.enqueued + metrics.sync_fallbacks + drops ==
			    (uint64_t)count &&
		    io.async_completed == metrics.enqueued &&
		    io.sync_completed == metrics.sync_fallbacks &&
		    io.emitted_records + io.failed_records ==
			    io.async_completed + io.sync_completed &&
		    diag.queue_storage_bytes != 0;

	if (valid)
		qsort(latency, count, sizeof(*latency), cmp_u64);
	double producer_seconds = (double)(producer_end - begin) / 1e9;
	double end_to_end_seconds = (double)(output_end - begin) / 1e9;
	struct rusage usage = { 0 };
	getrusage(RUSAGE_SELF, &usage);

	printf("{\"threads\":%" PRIu64, nt);
	printf(",\"records_per_thread\":%" PRIu64, nr);
	printf(",\"payload_bytes\":%" PRIu64, bytes);
	printf(",\"queue_capacity\":%" PRIu64, cap);
	printf(",\"attempted\":%zu", count);
	printf(",\"enqueued\":%" PRIu64, metrics.enqueued);
	printf(",\"emitted\":%" PRIu64, io.emitted_records);
	printf(",\"failed\":%" PRIu64, io.failed_records);
	printf(",\"dropped\":%" PRIu64, drops);
	printf(",\"sync_fallbacks\":%" PRIu64, metrics.sync_fallbacks);
	printf(",\"spill_exhaustions\":%" PRIu64, diag.spill_exhaustions);
	printf(",\"queue_storage_bytes\":%" PRIu64, diag.queue_storage_bytes);
	printf(",\"producer_wake_signals\":%" PRIu64,
	       diag.producer_wake_signals);
	printf(",\"worker_wait_count\":%" PRIu64, diag.worker_wait_count);
	printf(",\"producer_seconds\":%.9f,\"end_to_end_seconds\":%.9f",
	       producer_seconds, end_to_end_seconds);
	double producer_rate = 0.0;
	double output_rate = 0.0;
	if (producer_seconds > 0)
		producer_rate = count / producer_seconds;
	if (end_to_end_seconds > 0)
		output_rate = io.emitted_records / end_to_end_seconds;
	printf(",\"producer_logs_per_sec\":%.3f", producer_rate);
	printf(",\"end_to_end_logs_per_sec\":%.3f", output_rate);
	printf(",\"api_p50_ns\":%" PRIu64, valid ? pct(latency, count, 50) : 0);
	printf(",\"api_p95_ns\":%" PRIu64, valid ? pct(latency, count, 95) : 0);
	printf(",\"api_p99_ns\":%" PRIu64, valid ? pct(latency, count, 99) : 0);
	printf(",\"rss_peak_kib\":%ld", usage.ru_maxrss);
	printf(",\"user_cpu_sec\":%.6f",
	       usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6);
	printf(",\"system_cpu_sec\":%.6f",
	       usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6);
	printf(",\"valid\":%s}\n", valid ? "true" : "false");
	fflush(stdout);
	logger_destroy(logger);
	pthread_mutex_destroy(&gate.mutex);
	pthread_cond_destroy(&gate.cond);
	free(args);
	free(threads);
	free(latency);
	free(message);
	return valid ? 0 : 5;
}
