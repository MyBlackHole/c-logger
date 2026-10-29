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

/*
 * 此驱动仅依赖公开 API。每个输入槽位只尝试一次，不补偿丢弃；
 * 到达测量截止时间后停止发起调用，未发出的槽位归发生器缺失。
 */
struct phase {
	logger_t *logger;
	const char *payload;
	uint64_t rate;
	uint64_t count;
	uint64_t start;
	uint64_t end;
	unsigned int threads;
	unsigned int number;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready;
	uint64_t *calls;
	uint64_t *late;
	int64_t *status;
};

struct producer {
	struct phase *phase;
	unsigned int index;
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
		int status = logger_log(p->logger, LOGGER_INFO, "fixed_rate",
					__FILE__, __LINE__, __func__,
					"FR phase=%u seq=%" PRIu64
					" payload=%s",
					p->number, i, p->payload);
		/*
		 * 此耗时包含时钟读取的测量开销。
		 * 调用包含格式化/入队/可能回退，不代表输出或落盘。
		 */
		p->calls[i] = now_ns() - before;
		p->status[i] = status;
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

static uint64_t percentile(const uint64_t *values, uint64_t n, unsigned int pct)
{
	return n ? values[(n * pct + 99) / 100 - 1] : 0;
}

struct latency {
	uint64_t p50;
	uint64_t p95;
	uint64_t p99;
	uint64_t max;
};

static struct latency summarize(uint64_t *values, uint64_t n)
{
	qsort(values, (size_t)n, sizeof(*values), compare_u64);
	return (struct latency){ percentile(values, n, 50),
				 percentile(values, n, 95),
				 percentile(values, n, 99),
				 n ? values[n - 1] : 0 };
}

static void print_success_latency(struct latency values, uint64_t n)
{
	if (!n) {
		printf("{\"count\":0,\"p50\":null,\"p95\":null,"
		       "\"p99\":null,\"max\":null}");
		return;
	}
	printf("{\"count\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p95\":%" PRIu64
	       ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64 "}",
	       n, values.p50, values.p95, values.p99, values.max);
}

static uint64_t drops(const logger_metrics_t *m)
{
	uint64_t n = 0;
	for (unsigned int i = 0; i < 6; i++)
		n += m->dropped[i];
	return n;
}

/*
 * 停止生产后仅轮询公开完成计数，1ms 轮询会抬高观测上界。
 * 不把 consumer_records（出队数）冒充输出完成数。
 */
static int wait_output(logger_t *logger, const logger_metrics_t *m)
{
	uint64_t deadline = now_ns() + 10000000000ull;
	for (;;) {
		logger_io_metrics_t io;
		int rc = logger_get_io_metrics(logger, &io);
		if (rc)
			return rc;
		if (io.async_completed == m->enqueued &&
		    io.sync_completed == m->sync_fallbacks)
			return 0;
		uint64_t current = now_ns();
		if (current >= deadline || sleep_until(current + 1000000ull))
			return -1;
	}
}

static int run_phase(logger_t *logger, unsigned int number,
		     unsigned int threads, uint64_t rate, unsigned int seconds,
		     const char *payload, int file_backend, FILE *raw)
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
	logger_metrics_t before;
	logger_metrics_t after;
	logger_io_metrics_t io_before;
	logger_io_metrics_t io_after;
	uint64_t attempted = 0;
	unsigned int started = 0;
	int failed = 0;
	int generator_error = 0;

	p.calls = calloc((size_t)p.count, sizeof(*p.calls));
	p.late = calloc((size_t)p.count, sizeof(*p.late));
	p.status = calloc((size_t)p.count, sizeof(*p.status));
	if (!p.calls || !p.late || !p.status) {
		failed = 1;
		goto cleanup;
	}
	/* 强制预触量测数组，避免把首次写入缺页开销放入发压窗口。 */
	for (uint64_t i = 0; i < p.count; i++) {
		((volatile uint64_t *)p.calls)[i] = 0;
		((volatile uint64_t *)p.late)[i] = 0;
		((volatile int64_t *)p.status)[i] = 0;
	}
	if (logger_get_metrics(logger, &before) ||
	    logger_get_io_metrics(logger, &io_before)) {
		failed = 1;
		goto cleanup;
	}
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
	for (unsigned int i = 0; i < started; i++) {
		if (pthread_join(ids[i], NULL))
			abort(); /* 不在借用者尚未退出时释放实例或数组。 */
		attempted += producers[i].attempted;
		if (producers[i].error)
			generator_error = producers[i].error;
	}
	uint64_t producer_end = now_ns();
	if (logger_get_metrics(logger, &after)) {
		failed = 1;
		goto cleanup;
	}
	int output_rc = wait_output(logger, &after);
	uint64_t output_end = now_ns();
	uint64_t flush_start = now_ns();
	/* /dev/null 没有 fsync 契约，不能借其 EINVAL 声称文件刷新失败。 */
	int flush_rc = file_backend ? logger_flush(logger) : 0;
	int flush_error = -flush_rc;
	uint64_t flush_ns = now_ns() - flush_start;
	if (logger_get_io_metrics(logger, &io_after)) {
		failed = 1;
		goto cleanup;
	}
	uint64_t enqueued = after.enqueued - before.enqueued;
	uint64_t fallback = after.sync_fallbacks - before.sync_fallbacks;
	uint64_t dropped = drops(&after) - drops(&before);
	uint64_t emitted = io_after.emitted_records - io_before.emitted_records;
	uint64_t io_failed = io_after.failed_records - io_before.failed_records;
	uint64_t completed =
		io_after.async_completed - io_before.async_completed;
	uint64_t sync_completed =
		io_after.sync_completed - io_before.sync_completed;
	uint64_t api_success = 0;
	uint64_t api_eagain = 0;
	uint64_t api_other_errors = 0;
	for (uint64_t i = 0; i < p.count; i++) {
		if (!p.calls[i])
			continue;
		if (!p.status[i])
			api_success++;
		else if (p.status[i] == -EAGAIN)
			api_eagain++;
		else
			api_other_errors++;
	}
	/*
	 * 本负载不进行级别过滤；负返回可能已输出，不能据此重试。
	 * 其他错误使样本失败，仍保留原始返回值和计数供调查。
	 */
	int accounting =
		attempted == api_success + api_eagain + api_other_errors &&
		api_success == enqueued + fallback && api_eagain == dropped &&
		completed == enqueued && sync_completed == fallback &&
		emitted + io_failed == completed + sync_completed;
	/*
	 * 原始数组按计划槽位保留；calls==0 表示未发出。机器本地字节序，
	 * 由 runner 连同平台信息存档，不能把缺失槽位作为零延迟。
	 */
	uint64_t header[2] = { number, p.count };
	int raw_ok = fwrite(header, sizeof(*header), 2, raw) == 2 &&
		     fwrite(p.calls, sizeof(*p.calls), (size_t)p.count, raw) ==
			     p.count &&
		     fwrite(p.late, sizeof(*p.late), (size_t)p.count, raw) ==
			     p.count &&
		     fwrite(p.status, sizeof(*p.status), (size_t)p.count,
			    raw) == p.count &&
		     fflush(raw) == 0;
	uint64_t n = 0;
	uint64_t late_over_1ms = 0;
	for (uint64_t i = 0; i < p.count; i++) {
		if (!p.calls[i])
			continue;
		if (p.late[i] > 1000000ull)
			late_over_1ms++;
		p.late[n++] = p.late[i];
	}
	struct latency late_values = summarize(p.late, n);
	/* 原件已写出；复用迟到数组计算成功调用分位，不新增发压分配。 */
	uint64_t success_n = 0;
	for (uint64_t i = 0; i < p.count; i++) {
		if (p.calls[i] && !p.status[i])
			p.late[success_n++] = p.calls[i];
	}
	struct latency success_values = summarize(p.late, success_n);
	n = 0;
	for (uint64_t i = 0; i < p.count; i++) {
		if (p.calls[i])
			p.calls[n++] = p.calls[i];
	}
	qsort(p.calls, (size_t)n, sizeof(*p.calls), compare_u64);
	failed = generator_error || started != threads || output_rc ||
		 flush_rc || !accounting || io_failed || io_after.first_error ||
		 n != attempted || success_n != api_success ||
		 api_other_errors || !raw_ok;
	printf("{\"raw_schema\":2,\"api_success\":%" PRIu64
	       ",\"api_eagain\":%" PRIu64 ",\"api_other_errors\":%" PRIu64
	       ",\"api_success_ns\":",
	       api_success, api_eagain, api_other_errors);
	print_success_latency(success_values, success_n);
	printf(",\"phase\":%u,\"rate\":%" PRIu64 ",\"seconds\":%u,"
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
	       late_values.p50, late_values.p95, late_values.p99,
	       late_values.max, enqueued, dropped, fallback, emitted, io_failed,
	       after.queue_high_watermark,
	       producer_end > p.start ? (producer_end - p.start) / 1e9 : 0,
	       output_end > p.start ? (output_end - p.start) / 1e9 : 0,
	       output_end - producer_end, file_backend ? flush_ns : 0,
	       file_backend ? "true" : "false", flush_rc, flush_error,
	       accounting ? "true" : "false", failed ? "false" : "true");
cleanup:
	free(p.calls);
	free(p.late);
	free(p.status);
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
	if (logger_process_init() != 0)
		return 1;
	uint64_t threads;
	uint64_t rate;
	uint64_t seconds;
	uint64_t warmup;
	uint64_t size;
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
	/* 固定八字节版本头，拒绝把旧的双数组原件按三数组解释。 */
	static const unsigned char magic[8] = { 'C', 'L', 'F', 'R', 2, 0, 0, 0 };
	if (fwrite(magic, 1, sizeof(magic), raw) != sizeof(magic)) {
		fclose(raw);
		return 3;
	}
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
	config.level = LOGGER_INFO;
	config.file_path = argv[6];
	config.async_mode = 1;
	config.queue_capacity = 65536;
	config.rotation.mode = LOGGER_ROTATE_NONE;
	config.flush_level = LOGGER_OFF;
	config.overflow[LOGGER_INFO] = LOGGER_OVERFLOW_DROP;
	logger_t *logger = NULL;
	int create_rc = logger_create(&config, &logger);
	if (create_rc) {
		fprintf(stderr, "{\"error\":\"logger_create\",\"errno\":%d}\n",
			-create_rc);
		fclose(raw);
		return 3;
	}
	int file_backend = strcmp(argv[6], "/dev/null") != 0;
	int rc = warmup ? run_phase(logger, 0, (unsigned int)threads, rate,
				    (unsigned int)warmup, payload, file_backend,
				    raw) :
			  0;
	if (!rc)
		rc = run_phase(logger, 1, (unsigned int)threads, rate,
			       (unsigned int)seconds, payload, file_backend,
			       raw);
	/*
	 * 销毁 /dev/null 时的 fsync EINVAL 不属于输出失败，单独记录状态。
	 * 文件后端则必须成功关闭。
	 */
	int close_rc = logger_destroy(&logger);
	int close_errno = -close_rc;
	printf("{\"close_rc\":%d,\"close_errno\":%d}\n", close_rc, close_errno);
	int raw_close_rc = fclose(raw);
	return rc || raw_close_rc ||
			       (close_rc &&
				(file_backend || close_errno != EINVAL)) ?
		       4 :
		       0;
}
