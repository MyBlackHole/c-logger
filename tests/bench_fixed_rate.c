#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

/* 此驱动仅依赖公开 API。每个输入槽位只尝试一次，不补偿丢弃；
 * 到达测量截止时间后停止发起调用，未发出的槽位归发生器缺失。 */
struct phase {
	logger_t *logger;
	const char *payload;
	uint64_t rate, count, start, end;
	unsigned threads, number;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready;
	uint64_t *calls, *late;
};

struct producer {
	struct phase *phase;
	unsigned index;
	uint64_t attempted;
	int error;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		abort();
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int sleep_until(uint64_t ns)
{
	struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000ull),
			       .tv_nsec = (long)(ns % 1000000000ull) };
	int rc;
	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	} while (rc == EINTR);
	return rc;
}

static void *produce(void *opaque)
{
	struct producer *producer = opaque;
	struct phase *p = producer->phase;

	pthread_mutex_lock(&p->mutex);
	while (!p->ready)
		pthread_cond_wait(&p->cond, &p->mutex);
	pthread_mutex_unlock(&p->mutex);
	if (p->ready < 0)
		return NULL;
	for (uint64_t i = producer->index; i < p->count; i += p->threads) {
		uint64_t due = p->start + i * 1000000000ull / p->rate;
		uint64_t before = now_ns();
		if (before < due) {
			producer->error = sleep_until(due);
			if (producer->error)
				break;
		}
		before = now_ns();
		if (before >= p->end)
			break;
		uint64_t late = before > due ? before - due : 0;
		logger_log(p->logger, LOGGER_INFO, "fixed_rate", __FILE__,
			   __LINE__, __func__,
			   "FR phase=%u seq=%" PRIu64 " payload=%s", p->number,
			   i, p->payload);
		/* 此耗时包含时钟读取的测量开销。
		 * 调用包含格式化/入队/可能回退，不代表输出或落盘。 */
		p->calls[i] = now_ns() - before;
		p->late[i] = late;
		if (!p->calls[i])
			p->calls[i] = 1;
		producer->attempted++;
	}
	return NULL;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *values, uint64_t n, unsigned pct)
{
	return n ? values[(n * pct + 99) / 100 - 1] : 0;
}

static uint64_t drops(const logger_metrics_t *m)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < 6; i++)
		n += m->dropped[i];
	return n;
}

/* 停止生产后仅轮询公开完成计数，1ms 轮询会抬高观测上界。
 * 不把 consumer_records（出队数）冒充输出完成数。 */
static int wait_output(logger_t *logger, const logger_metrics_t *m)
{
	uint64_t deadline = now_ns() + 10000000000ull;
	for (;;) {
		logger_io_metrics_t io;
		logger_get_io_metrics(logger, &io);
		if (io.async_completed == m->enqueued &&
		    io.sync_completed == m->sync_fallbacks)
			return 0;
		uint64_t current = now_ns();
		if (current >= deadline || sleep_until(current + 1000000ull))
			return -1;
	}
}

static int run_phase(logger_t *logger, unsigned number, unsigned threads,
		     uint64_t rate, unsigned seconds, const char *payload,
		     int file_backend, FILE *raw)
{
	struct phase p = { .logger = logger,
			   .payload = payload,
			   .rate = rate,
			   .count = rate * seconds,
			   .threads = threads,
			   .number = number,
			   .mutex = PTHREAD_MUTEX_INITIALIZER,
			   .cond = PTHREAD_COND_INITIALIZER };
	pthread_t ids[64];
	struct producer producers[64] = { 0 };
	logger_metrics_t before, after;
	logger_io_metrics_t io_before, io_after;
	uint64_t attempted = 0;
	unsigned started = 0;
	int failed = 0;
	int generator_error = 0;

	p.calls = calloc((size_t)p.count, sizeof(*p.calls));
	p.late = calloc((size_t)p.count, sizeof(*p.late));
	if (!p.calls || !p.late) {
		failed = 1;
		goto cleanup;
	}
	/* 强制预触量测数组，避免把首次写入缺页开销放入发压窗口。 */
	for (uint64_t i = 0; i < p.count; i++) {
		((volatile uint64_t *)p.calls)[i] = 0;
		((volatile uint64_t *)p.late)[i] = 0;
	}
	logger_get_metrics(logger, &before);
	logger_get_io_metrics(logger, &io_before);
	for (; started < threads; started++) {
		producers[started].phase = &p;
		producers[started].index = started;
		if (pthread_create(&ids[started], NULL, produce,
				   &producers[started]))
			break;
	}
	pthread_mutex_lock(&p.mutex);
	p.start = now_ns() + 20000000ull;
	p.end = p.start + (uint64_t)seconds * 1000000000ull;
	p.ready = started == threads ? 1 : -1;
	pthread_cond_broadcast(&p.cond);
	pthread_mutex_unlock(&p.mutex);
	for (unsigned i = 0; i < started; i++) {
		if (pthread_join(ids[i], NULL))
			abort(); /* 不在借用者尚未退出时释放实例或数组。 */
		attempted += producers[i].attempted;
		if (producers[i].error)
			generator_error = producers[i].error;
	}
	uint64_t producer_end = now_ns();
	logger_get_metrics(logger, &after);
	int output_rc = wait_output(logger, &after);
	uint64_t output_end = now_ns();
	uint64_t flush_start = now_ns();
	/* /dev/null 没有 fsync 契约，不能借其 EINVAL 声称文件刷新失败。 */
	int flush_rc = file_backend ? logger_flush_instance_status(logger) : 0;
	int flush_error = flush_rc ? errno : 0;
	uint64_t flush_ns = now_ns() - flush_start;
	logger_get_io_metrics(logger, &io_after);
	uint64_t enqueued = after.enqueued - before.enqueued;
	uint64_t fallback = after.sync_fallbacks - before.sync_fallbacks;
	uint64_t dropped = drops(&after) - drops(&before);
	uint64_t emitted = io_after.emitted_records - io_before.emitted_records;
	uint64_t io_failed = io_after.failed_records - io_before.failed_records;
	uint64_t completed =
		io_after.async_completed - io_before.async_completed;
	uint64_t sync_completed =
		io_after.sync_completed - io_before.sync_completed;
	int accounting = attempted == enqueued + fallback + dropped &&
			 completed == enqueued && sync_completed == fallback &&
			 emitted + io_failed == completed + sync_completed;
	/* 原始数组按计划槽位保留；calls==0 表示未发出。机器本地字节序，
	 * 由 runner 连同平台信息存档，不能把缺失槽位作为零延迟。 */
	uint64_t header[2] = { number, p.count };
	int raw_ok = fwrite(header, sizeof(*header), 2, raw) == 2 &&
		     fwrite(p.calls, sizeof(*p.calls), (size_t)p.count, raw) ==
			     p.count &&
		     fwrite(p.late, sizeof(*p.late), (size_t)p.count, raw) ==
			     p.count &&
		     fflush(raw) == 0;
	uint64_t n = 0;
	uint64_t late_over_1ms = 0;
	for (uint64_t i = 0; i < p.count; i++) {
		if (!p.calls[i])
			continue;
		if (p.late[i] > 1000000ull)
			late_over_1ms++;
		p.calls[n] = p.calls[i];
		p.late[n++] = p.late[i];
	}
	qsort(p.calls, (size_t)n, sizeof(*p.calls), compare_u64);
	qsort(p.late, (size_t)n, sizeof(*p.late), compare_u64);
	failed = generator_error || started != threads || output_rc ||
		 flush_rc || !accounting || io_failed || io_after.first_error ||
		 n != attempted || !raw_ok;
	printf("{\"phase\":%u,\"rate\":%" PRIu64 ",\"seconds\":%u,"
	       "\"start_monotonic_ns\":%" PRIu64
	       ",\"end_monotonic_ns\":%" PRIu64 ","
	       "\"threads\":%u,\"scheduled\":%" PRIu64 ",\"attempted\":%" PRIu64
	       ","
	       "\"generator_errno\":%d,\"generator_missed\":%" PRIu64
	       ",\"late_over_1ms\":%" PRIu64 ","
	       "\"api_ns\":{\"p50\":%" PRIu64 ",\"p95\":%" PRIu64
	       ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64 "},"
	       "\"arrival_late_ns\":{\"p50\":%" PRIu64 ",\"p95\":%" PRIu64
	       ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64 "},"
	       "\"enqueued\":%" PRIu64 ",\"dropped\":%" PRIu64
	       ",\"sync_fallbacks\":%" PRIu64 ","
	       "\"emitted\":%" PRIu64 ",\"failed\":%" PRIu64
	       ",\"queue_hwm_lifetime\":%" PRIu64 ","
	       "\"producer_seconds\":%.9f,\"output_observed_seconds\":%.9f,"
	       "\"output_drain_observed_ns\":%" PRIu64 ",\"flush_ns\":%" PRIu64
	       ","
	       "\"flush_applicable\":%s,\"flush_rc\":%d,\"flush_errno\":%d,"
	       "\"accounting_valid\":%s,\"valid\":%s}\n",
	       number, rate, seconds, p.start, p.end, threads, p.count,
	       attempted, generator_error, p.count - attempted, late_over_1ms,
	       percentile(p.calls, n, 50), percentile(p.calls, n, 95),
	       percentile(p.calls, n, 99), n ? p.calls[n - 1] : 0,
	       percentile(p.late, n, 50), percentile(p.late, n, 95),
	       percentile(p.late, n, 99), n ? p.late[n - 1] : 0, enqueued,
	       dropped, fallback, emitted, io_failed,
	       after.queue_high_watermark,
	       producer_end > p.start ? (producer_end - p.start) / 1e9 : 0,
	       output_end > p.start ? (output_end - p.start) / 1e9 : 0,
	       output_end - producer_end, file_backend ? flush_ns : 0,
	       file_backend ? "true" : "false", flush_rc, flush_error,
	       accounting ? "true" : "false", failed ? "false" : "true");
cleanup:
	free(p.calls);
	free(p.late);
	pthread_cond_destroy(&p.cond);
	pthread_mutex_destroy(&p.mutex);
	return failed;
}

static int number(const char *s, uint64_t *n)
{
	char *end;
	if (!*s || *s == '-')
		return -1;
	errno = 0;
	*n = strtoull(s, &end, 10);
	return errno || *end ? -1 : 0;
}

int main(int argc, char **argv)
{
	uint64_t threads, rate, seconds, warmup, size;
	if (argc != 8 || number(argv[1], &threads) || number(argv[2], &rate) ||
	    number(argv[3], &seconds) || number(argv[4], &warmup) ||
	    number(argv[5], &size) || threads < 1 || threads > 64 || rate < 1 ||
	    rate > 2000000 || seconds < 1 || seconds > 30 || warmup > 5 ||
	    size < 1 || size > 3500 || rate * (seconds + warmup) > 2000000 ||
	    rate * (seconds + warmup) * (size + 512) > 1073741824ull) {
		fprintf(stderr,
			"{\"error\":\"invalid_arguments_or_budget\"}\n");
		return 2;
	}
	/* 不接收已有普通文件，避免把旧日志误认为本次样本。 */
	struct stat st;
	if (strcmp(argv[6], "/dev/null") &&
	    (lstat(argv[6], &st) == 0 || errno != ENOENT))
		return 2;
	FILE *raw = fopen(argv[7], "wbx");
	if (!raw)
		return 3;
	/* 时钟对的控制样本仅描述测量下限，不从 API 分位中相减。 */
	uint64_t clock_pairs[1000];
	for (size_t i = 0; i < 1000; i++) {
		uint64_t begin = now_ns();
		clock_pairs[i] = now_ns() - begin;
	}
	qsort(clock_pairs, 1000, sizeof(*clock_pairs), compare_u64);
	printf("{\"clock_pair_ns\":{\"p50\":%" PRIu64 ",\"p99\":%" PRIu64
	       "}}\n",
	       percentile(clock_pairs, 1000, 50),
	       percentile(clock_pairs, 1000, 99));
	char payload[3501];
	memset(payload, 'x', (size_t)size);
	payload[size] = '\0';
	logger_config_t config = LOGGER_DEFAULT_CONFIG();
	config.outputs = LOGGER_OUT_FILE;
	config.file_path = argv[6];
	config.async_mode = 1;
	config.queue_capacity = 65536;
	config.rotation.mode = LOGGER_ROTATE_NONE;
	config.flush_level = LOGGER_OFF;
	config.overflow[LOGGER_INFO] = LOGGER_OVERFLOW_DROP;
	logger_t *logger = logger_create(&config);
	if (!logger) {
		fprintf(stderr, "{\"error\":\"logger_create\",\"errno\":%d}\n",
			errno);
		fclose(raw);
		return 3;
	}
	int file_backend = strcmp(argv[6], "/dev/null") != 0;
	int rc = warmup ? run_phase(logger, 0, (unsigned)threads, rate,
				    (unsigned)warmup, payload, file_backend,
				    raw) :
			  0;
	if (!rc)
		rc = run_phase(logger, 1, (unsigned)threads, rate,
			       (unsigned)seconds, payload, file_backend, raw);
	/* 销毁 /dev/null 时的 fsync EINVAL 不属于输出失败，单独记录状态。
	 * 文件后端则必须成功关闭。 */
	int close_rc = logger_destroy_status(logger);
	int close_errno = close_rc ? errno : 0;
	printf("{\"close_rc\":%d,\"close_errno\":%d}\n", close_rc, close_errno);
	int raw_close_rc = fclose(raw);
	return rc || raw_close_rc ||
			       (close_rc &&
				(file_backend || close_errno != EINVAL)) ?
		       4 :
		       0;
}
