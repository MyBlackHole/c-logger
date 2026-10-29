#define _GNU_SOURCE
#include "logger.h"
#include "support.h"
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>

/*
 * 不进入 TSan full-static 集合：GCC TSan 不支持多线程 fork 后建线程。
 * 独立普通/ASan 验证真实生产静态和共享库，不修改生产故障开关。
 */
enum {
	PARENT_THREADS = 2,
	CHILDREN = 4,
	CHILD_RECORDS = 32,
	PARENT_LIMIT = 10000
};

struct parent_writer {
	unsigned int number;

	_Atomic unsigned int written;
};

struct child_writer {
	logger_t *logger;
	int failure;
};

static logger_t *parent_owner;
static _Atomic int ready;
static _Atomic int stop_writers;

static logger_config_t file_config(const char *name)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.outputs = LOGGER_OUT_FILE;
	c.file_path = name;
	c.async_mode = 1;
	c.queue_capacity = 8192;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	return c;
}

static void *parent_write(void *arg)
{
	struct parent_writer *writer = arg;

	atomic_fetch_add_explicit(&ready, 1, memory_order_release);
	for (unsigned int i = 0; i < PARENT_LIMIT; ++i) {
		if (atomic_load(&stop_writers))
			break;
		CHECK(LOGGER_INFO(parent_owner, "fork-stress",
				  "PARENT_WRITER=%u:%u", writer->number,
				  i) == 0);
		atomic_fetch_add_explicit(&writer->written, 1,
					  memory_order_release);
		struct timespec delay = { 0, 1000000 };
		(void)nanosleep(&delay, NULL);
	}
	while (!atomic_load_explicit(&stop_writers, memory_order_acquire))
		sched_yield();
	return NULL;
}

static void *child_write(void *arg)
{
	struct child_writer *writer = arg;

	for (unsigned int i = 0; i < CHILD_RECORDS; ++i) {
		int rc = LOGGER_INFO(writer->logger, "fork-child",
				     "CHILD_WORKER=%u", i);
		if (rc) {
			writer->failure = 1;
			return NULL;
		}
	}
	return NULL;
}

/*
 * 不是仅检查线程已启动：每次 fork 前后都要求两个业务写者
 * 确实完成了新日志调用。计数由 release 发布、acquire 观测。
 */
static unsigned int parent_written(const struct parent_writer *writer)
{
	return atomic_load_explicit(&writer->written, memory_order_acquire);
}

static void wait_parent_progress(const struct parent_writer *writers,
				 const unsigned int baseline[PARENT_THREADS])
{
	for (unsigned int retry = 0; retry < 5000; ++retry) {
		int advanced = 1;

		for (unsigned int i = 0; i < PARENT_THREADS; ++i) {
			unsigned int value = parent_written(&writers[i]);

			if (value <= baseline[i])
				advanced = 0;
		}
		if (advanced)
			return;
		nap_ms();
	}
	CHECK(!"parent_writers_stalled_across_fork");
}

static void child_expect(int valid, int code)
{
	if (!valid)
		_exit(code);
}

static void child_run(unsigned int number)
{
	errno = EDOM;
	child_expect(LOGGER_INFO(parent_owner, "old",
				 "OLD_PARENT_MUST_NOT_APPEAR") == -ECHILD &&
			     errno == EDOM,
		     21);
	child_expect(logger_process_init() == 0, 22);
	char path[48];
	int path_len = snprintf(path, sizeof(path), "child-%u.log", number);

	child_expect(path_len > 0, 23);
	logger_config_t cfg = file_config(path);
	logger_t *own = NULL;

	child_expect(logger_create(&cfg, &own) == 0 && own, 24);
	int begin_rc = LOGGER_INFO(own, "child", "CHILD_BEGIN=%u", number);

	child_expect(begin_rc == 0, 25);
	struct child_writer writer = { .logger = own };
	pthread_t producer;

	child_expect(pthread_create(&producer, NULL, child_write, &writer) == 0,
		     26);
	child_expect(pthread_join(producer, NULL) == 0 && !writer.failure, 27);
	child_expect(LOGGER_INFO(own, "child", "CHILD_END=%u", number) == 0,
		     28);
	child_expect(logger_flush(own) == 0, 29);
	logger_io_metrics_t io;

	child_expect(logger_get_io_metrics(own, &io) == 0, 30);
	child_expect(io.emitted_records == CHILD_RECORDS + 2 &&
			     io.failed_records == 0,
		     31);
	logger_metrics_t metrics;

	child_expect(logger_get_metrics(own, &metrics) == 0, 32);
	child_expect(metrics.dropped[LOGGER_INFO] == 0, 33);
	child_expect(logger_destroy(&own) == 0 && own == NULL, 34);
	_exit(0);
}

static void wait_child(pid_t pid)
{
	int status = 0;
	pid_t got;

	do {
		got = waitpid(pid, &status, 0);
	} while (got == -1 && errno == EINTR);
	if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		fprintf(stderr, "CHILD_PID=%ld STATUS=%d\n", (long)pid, status);
	CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned int count_lines(const char *path, const char *marker)
{
	FILE *file = fopen(path, "r");

	CHECK(file != NULL);
	char line[8192];
	unsigned int count = 0;

	while (fgets(line, sizeof(line), file)) {
		CHECK(strstr(line, marker) != NULL);
		++count;
	}
	CHECK(!ferror(file) && !fclose(file));
	return count;
}

static void remove_log(const char *path)
{
	CHECK(unlink(path) == 0);
	char lock[96];

	CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", path) > 0);
	CHECK(unlink(lock) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(!strcmp(argv[1], "serial") || !strcmp(argv[1], "parallel"));
	alarm(80);
	char directory[] = "/tmp/logger-fork-worker-stress-XXXXXX";

	enter_temp(directory);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = file_config("parent.log");

	CHECK(logger_create(&cfg, &parent_owner) == 0 && parent_owner);
	CHECK(LOGGER_INFO(parent_owner, "parent", "PARENT_BEGIN") == 0);

	struct parent_writer writers[PARENT_THREADS] = { 0 };

	for (unsigned int i = 0; i < PARENT_THREADS; ++i)
		writers[i].number = i;
	pthread_t threads[PARENT_THREADS];

	for (unsigned int i = 0; i < PARENT_THREADS; ++i)
		CHECK(pthread_create(&threads[i], NULL, parent_write,
				     &writers[i]) == 0);
	for (unsigned int wait = 0; wait < 5000; ++wait) {
		if (atomic_load(&ready) == PARENT_THREADS)
			break;
		nap_ms();
	}
	CHECK(atomic_load(&ready) == PARENT_THREADS);
	/* ready 只代表线程已启动；首次 fork 前必须有真实成功写入。 */
	unsigned int initial[PARENT_THREADS] = { 0 };

	wait_parent_progress(writers, initial);

	pid_t children[CHILDREN];
	unsigned int progress_checks = 0;
	int parallel = !strcmp(argv[1], "parallel");

	for (unsigned int i = 0; i < CHILDREN; ++i) {
		CHECK(atomic_load(&ready) == PARENT_THREADS);
		CHECK(!atomic_load(&stop_writers));
		unsigned int before[PARENT_THREADS];

		for (unsigned int j = 0; j < PARENT_THREADS; ++j) {
			before[j] = parent_written(&writers[j]);
			CHECK(before[j] > 0 && before[j] < PARENT_LIMIT);
		}
		/*
		 * 此管道属于测试宿主，而非 Logger。子进程在重建域前
		 * 只做 close/read 等异步信号安全操作；父进程确认真实
		 * 写入进度后才允许其调用 logger_process_init。
		 */
		int gate[2];

		CHECK(pipe(gate) == 0);
		children[i] = fork();
		CHECK(children[i] >= 0);
		if (!children[i]) {
			child_expect(close(gate[1]) == 0, 40);
			char token = 0;
			ssize_t n;

			do {
				n = read(gate[0], &token, 1);
			} while (n < 0 && errno == EINTR);
			child_expect(n == 1 && token == 'G', 41);
			child_expect(close(gate[0]) == 0, 42);
			child_run(i);
		}
		CHECK(close(gate[0]) == 0);
		wait_parent_progress(writers, before);
		progress_checks += PARENT_THREADS;
		char token = 'G';

		CHECK(write(gate[1], &token, 1) == 1);
		CHECK(close(gate[1]) == 0);
		for (unsigned int j = 0; j < PARENT_THREADS; ++j)
			before[j] = parent_written(&writers[j]);
		wait_parent_progress(writers, before);
		progress_checks += PARENT_THREADS;
		if (!parallel)
			wait_child(children[i]);
	}
	if (parallel) {
		for (unsigned int i = 0; i < CHILDREN; ++i)
			wait_child(children[i]);
	}
	atomic_store(&stop_writers, 1);
	for (unsigned int i = 0; i < PARENT_THREADS; ++i)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(parent_written(&writers[0]) && parent_written(&writers[1]));
	CHECK(LOGGER_INFO(parent_owner, "parent", "PARENT_END") == 0);
	CHECK(logger_flush(parent_owner) == 0);
	uint64_t emitted = 2;

	for (unsigned int i = 0; i < PARENT_THREADS; ++i)
		emitted += parent_written(&writers[i]);
	logger_io_metrics_t io;

	CHECK(logger_get_io_metrics(parent_owner, &io) == 0);
	CHECK(io.emitted_records == emitted && io.failed_records == 0);
	logger_metrics_t metrics;

	CHECK(logger_get_metrics(parent_owner, &metrics) == 0);
	CHECK(!metrics.dropped[LOGGER_INFO]);
	CHECK(logger_destroy(&parent_owner) == 0 && !parent_owner);
	CHECK(count_lines("parent.log", "PARENT_") == emitted);
	CHECK(!file_contains("parent.log", "CHILD_"));
	for (unsigned int i = 0; i < CHILDREN; ++i) {
		char path[48];

		CHECK(snprintf(path, sizeof(path), "child-%u.log", i) > 0);
		CHECK(count_lines(path, "CHILD_") == CHILD_RECORDS + 2);
		CHECK(!file_contains(path, "PARENT_"));
		CHECK(file_contains(path, "CHILD_WORKER=31"));
		remove_log(path);
	}
	remove_log("parent.log");
	CHECK(chdir("/") == 0);
	CHECK(rmdir(directory) == 0);
	printf("FORK_WORKER_STRESS_PASS=%s children=%u parent_records=%" PRIu64
	       " child_records=%u parent_progress_checks=%u\n",
	       argv[1], CHILDREN, emitted, CHILDREN * (CHILD_RECORDS + 2),
	       progress_checks);
	return 0;
}
