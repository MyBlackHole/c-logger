#define _GNU_SOURCE
#include "global_support.h"

/* The real-DSO variant cannot inspect private implementation counters. It
 * checks observable descriptor cleanup and shutdown state instead; static
 * white-box tests continue to assert the original private counter. */
#ifdef LOGGER_PUBLIC_ABI_TEST
#include <dirent.h>
static unsigned initial_fds;
static unsigned open_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	unsigned n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			++n;
	CHECK(!closedir(d));
	return n;
}
#endif
static void expect_clean(void)
{
#ifdef LOGGER_PUBLIC_ABI_TEST
	CHECK(open_fds() == initial_fds);
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
#else
	CHECK(!logger_process_object_count());
#endif
}

static _Atomic int running = 1;
static _Atomic unsigned long calls;
static pthread_barrier_t init_barrier, run_barrier;
static _Atomic int ready[8];
static _Atomic int succeeded, already;
static logger_config_t cfg;
static void *initializer(void *unused)
{
	(void)unused;
	int rc = pthread_barrier_wait(&init_barrier);
	CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
	if (!logger_init(&cfg))
		atomic_fetch_add(&succeeded, 1);
	else if (errno == EALREADY)
		atomic_fetch_add(&already, 1);
	else
		CHECK(!"unexpected init error");
	return NULL;
}
static void *producer(void *ptr)
{
	size_t id = (size_t)ptr;
	int rc = pthread_barrier_wait(&run_barrier);
	CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
	while (atomic_load(&running)) {
		LOG_INFO("producer=%zu", id);
		atomic_fetch_add(&calls, 1);
		atomic_store(&ready[id], 1);
	}
	return NULL;
}
static void *controller(void *ptr)
{
	size_t id = (size_t)ptr;
	int br = pthread_barrier_wait(&run_barrier);
	CHECK(br == 0 || br == PTHREAD_BARRIER_SERIAL_THREAD);
	while (atomic_load(&running)) {
		logger_metrics_t m;
		logger_io_metrics_t io;
		logger_get_global_metrics(&m);
		logger_get_global_io_metrics(&io);
		(void)logger_global_dropped();
		logger_set_level(LOGGER_INFO);
		int rc = logger_flush_status();
		CHECK(rc == 0 ||
		      (rc == -1 && (errno == ESHUTDOWN || errno == EAGAIN ||
				    errno == ENODEV)));
		rc = logger_reopen();
		CHECK(rc == 0 ||
		      (rc == -1 && (errno == ESHUTDOWN || errno == EAGAIN ||
				    errno == ENODEV)));
		atomic_fetch_add(&calls, 1);
		atomic_store(&ready[id], 1);
	}
	return NULL;
}
static void init_race(void)
{
	cfg = config_for("old.log", 1);
	for (int round = 0; round < 10; ++round) {
		pthread_t ts[8];
		atomic_store(&succeeded, 0);
		atomic_store(&already, 0);
		CHECK(!pthread_barrier_init(&init_barrier, NULL, 8));
		for (int i = 0; i < 8; ++i)
			CHECK(!pthread_create(&ts[i], NULL, initializer, NULL));
		for (int i = 0; i < 8; ++i)
			CHECK(!pthread_join(ts[i], NULL));
		CHECK(atomic_load(&succeeded) == 1 &&
		      atomic_load(&already) == 7);
		CHECK(!pthread_barrier_destroy(&init_barrier));
		LOG_INFO("winner-round=%d", round);
		CHECK(!stop_status());
		expect_clean();
	}
	CHECK(file_lines("old.log") == 10);
}
static void lifetimes(int async)
{
	cfg = config_for("old.log", async);
	CHECK(!logger_init(&cfg));
	pthread_t threads[8];
	CHECK(!pthread_barrier_init(&run_barrier, NULL, 9));
	for (size_t i = 0; i < 4; ++i)
		CHECK(!pthread_create(&threads[i], NULL, producer, (void *)i));
	for (size_t i = 4; i < 8; ++i)
		CHECK(!pthread_create(&threads[i], NULL, controller,
				      (void *)i));
	int br = pthread_barrier_wait(&run_barrier);
	CHECK(br == 0 || br == PTHREAD_BARRIER_SERIAL_THREAD);
	/* A fast controller loop must not finish before test participants ran. */
	for (int i = 0; i < 8; ++i)
		wait_flag(&ready[i]);
	for (int round = 0; round < 40; ++round) {
		LOG_INFO("owner-before-stop=%d", round);
		CHECK(!stop_status());
		expect_clean();
		CHECK(!logger_init(&cfg));
		LOG_INFO("owner-after-start=%d", round);
	}
	atomic_store(&running, 0);
	for (int i = 0; i < 8; ++i)
		CHECK(!pthread_join(threads[i], NULL));
	CHECK(atomic_load(&calls) >= 8);
	CHECK(!pthread_barrier_destroy(&run_barrier));
	CHECK(!logger_flush_status() && !stop_status());
	expect_clean();
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	fixture_reset();
#ifdef LOGGER_PUBLIC_ABI_TEST
	initial_fds = open_fds();
#endif
	if (!strcmp(argv[1], "init-race"))
		init_race();
	else if (!strcmp(argv[1], "sync"))
		lifetimes(0);
	else if (!strcmp(argv[1], "async"))
		lifetimes(1);
	else
		CHECK(!"unknown stress mode");
	puts("global concurrent control and repeated lifetimes passed");
	return 0;
}
