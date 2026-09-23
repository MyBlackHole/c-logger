#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <fcntl.h>
static _Atomic int pause_close, entered, released, init_attempted,
	init_finished;
static _Thread_local int initializer;
static logger_config_t cfg;
int __real_logger_destroy_status(logger_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_logger_destroy_status(logger_t *l)
{
	if (atomic_exchange(&pause_close, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&released);
	}
	return __real_logger_destroy_status(l);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *m)
{
	if (initializer) {
		initializer = 0;
		atomic_store(&init_attempted, 1);
	}
	return __real_pthread_mutex_lock(m);
}
static void *shutdown_thread(void *p)
{
	(void)p;
	logger_shutdown();
	return NULL;
}
static void *init_thread(void *p)
{
	(void)p;
	initializer = 1;
	CHECK(logger_init(&cfg) == 0);
	atomic_store(&init_finished, 1);
	return NULL;
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "global.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(logger_init(&cfg) == 0);
	LOG_INFO("old");
	if (!strcmp(argv[1], "repeat-no-effects")) {
		logger_config_t other = cfg;
		other.file_path = "must-not-exist.log";
		CHECK(logger_init(&other) == -1 && errno == EALREADY);
		CHECK(access("must-not-exist.log", F_OK) != 0 &&
		      access("must-not-exist.log.logger.lock", F_OK) != 0);
		logger_shutdown();
		return 0;
	}
	CHECK(!strcmp(argv[1], "teardown"));
	atomic_store(&pause_close, 1);
	pthread_t stop, init;
	CHECK(pthread_create(&stop, NULL, shutdown_thread, NULL) == 0);
	wait_flag(&entered);
	CHECK(pthread_create(&init, NULL, init_thread, NULL) == 0);
	wait_flag(&init_attempted);
	for (int i = 0; i < 20; ++i)
		nap_ms();
	CHECK(!atomic_load(&init_finished));
	logger_t *conflict = logger_create(&cfg);
	CHECK(!conflict && errno == EBUSY);
	atomic_store(&released, 1);
	CHECK(pthread_join(stop, NULL) == 0 && pthread_join(init, NULL) == 0);
	CHECK(atomic_load(&init_finished));
	LOG_INFO("new");
	CHECK(logger_flush_status() == 0);
	logger_shutdown();
	CHECK(file_contains("global.log", "old") &&
	      file_contains("global.log", "new"));
	return 0;
}
