#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>

/* 不进入 TSan full-static 集合：GCC TSan 不支持多线程 fork 后建线程。
 * 独立普通/ASan 验证真实生产静态和共享库，不修改生产故障开关。
 */
enum {
	PARENT_THREADS = 2,
	CHILDREN = 4,
	CHILD_RECORDS = 32,
	PARENT_LIMIT = 10000
};
struct parent_writer {
	unsigned number;
	unsigned written;
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
	for (unsigned i = 0; i < PARENT_LIMIT &&
			     !atomic_load_explicit(&stop_writers,
						  memory_order_acquire);
	     ++i) {
		CHECK(LOGGER_INFO(parent_owner, "fork-stress",
				  "PARENT_WRITER=%u:%u", writer->number,
				  i) == 0);
		++writer->written;
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
	for (unsigned i = 0; i < CHILD_RECORDS; ++i) {
		if (LOGGER_INFO(writer->logger, "fork-child",
				"CHILD_WORKER=%u", i)) {
			writer->failure = 1;
			return NULL;
		}
	}
	return NULL;
}

static void child_expect(int valid, int code)
{
	if (!valid)
		_exit(code);
}

static void child_run(unsigned number)
{
	errno = EDOM;
	child_expect(LOGGER_INFO(parent_owner, "old",
				 "OLD_PARENT_MUST_NOT_APPEAR") == -ECHILD &&
			     errno == EDOM,
		     21);
	child_expect(logger_process_init() == 0, 22);
	char path[48];
	child_expect(snprintf(path, sizeof(path), "child-%u.log", number) >
			     0,
		     23);
	logger_config_t cfg = file_config(path);
	logger_t *own = NULL;
	child_expect(logger_create(&cfg, &own) == 0 && own, 24);
	child_expect(LOGGER_INFO(own, "child", "CHILD_BEGIN=%u",
				 number) == 0,
		     25);
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

static unsigned count_lines(const char *path, const char *marker)
{
	FILE *file = fopen(path, "r");
	CHECK(file != NULL);
	char line[8192];
	unsigned count = 0;
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

	struct parent_writer writers[PARENT_THREADS] = {
		{ .number = 0 }, { .number = 1 }
	};
	pthread_t threads[PARENT_THREADS];
	for (unsigned i = 0; i < PARENT_THREADS; ++i)
		CHECK(pthread_create(&threads[i], NULL, parent_write,
				     &writers[i]) == 0);
	for (unsigned wait = 0; wait < 5000 && atomic_load(&ready) !=
					     PARENT_THREADS;
	     ++wait)
		nap_ms();
	CHECK(atomic_load(&ready) == PARENT_THREADS);

	pid_t children[CHILDREN];
	int parallel = !strcmp(argv[1], "parallel");
	for (unsigned i = 0; i < CHILDREN; ++i) {
		CHECK(atomic_load(&ready) == PARENT_THREADS);
		CHECK(!atomic_load(&stop_writers));
		children[i] = fork();
		CHECK(children[i] >= 0);
		if (!children[i])
			child_run(i);
		if (!parallel)
			wait_child(children[i]);
	}
	if (parallel) {
		for (unsigned i = 0; i < CHILDREN; ++i)
			wait_child(children[i]);
	}
	atomic_store(&stop_writers, 1);
	for (unsigned i = 0; i < PARENT_THREADS; ++i)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(writers[0].written && writers[1].written);
	CHECK(LOGGER_INFO(parent_owner, "parent", "PARENT_END") == 0);
	CHECK(logger_flush(parent_owner) == 0);
	uint64_t emitted = 2;
	for (unsigned i = 0; i < PARENT_THREADS; ++i)
		emitted += writers[i].written;
	logger_io_metrics_t io;
	CHECK(logger_get_io_metrics(parent_owner, &io) == 0);
	CHECK(io.emitted_records == emitted && io.failed_records == 0);
	logger_metrics_t metrics;
	CHECK(logger_get_metrics(parent_owner, &metrics) == 0);
	CHECK(!metrics.dropped[LOGGER_INFO]);
	CHECK(logger_destroy(&parent_owner) == 0 && !parent_owner);
	CHECK(count_lines("parent.log", "PARENT_") == emitted);
	CHECK(!file_contains("parent.log", "CHILD_"));
	for (unsigned i = 0; i < CHILDREN; ++i) {
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
	       " child_records=%u\n",
	       argv[1], CHILDREN, emitted, CHILDREN * (CHILD_RECORDS + 2));
	return 0;
}
