#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <fcntl.h>

static _Atomic int pause_close, entered, released;
static logger_t *owner;
static int destroy_result;
int __real_logger_destroy(logger_t **slot);
int __wrap_logger_destroy(logger_t **slot)
{
	if (atomic_exchange(&pause_close, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&released);
	}
	return __real_logger_destroy(slot);
}
/* 析构由宿主承担，默认解绑既不销毁文件，也不替宿主序列化后继创建。 */
static void *destroy_thread(void *unused)
{
	(void)unused;
	destroy_result = logger_destroy(&owner);
	return NULL;
}
int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0 && argc == 2);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "global.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	LOG_INFO("old");
	if (!strcmp(argv[1], "repeat-no-effects")) {
		CHECK(logger_default_set(NULL, owner) == -ESTALE);
		CHECK(logger_default_set(owner, owner) == 0);
		CHECK(logger_process_object_count() == 1);
		CHECK(logger_destroy(&owner) == -EBUSY && owner);
		CHECK(logger_default_set(owner, NULL) == 0);
		CHECK(logger_destroy(&owner) == 0);
		return 0;
	}
	CHECK(!strcmp(argv[1], "teardown"));
	CHECK(logger_default_set(owner, NULL) == 0);
	atomic_store(&pause_close, 1);
	pthread_t t;
	CHECK(pthread_create(&t, NULL, destroy_thread, NULL) == 0);
	wait_flag(&entered);
	logger_t *next = NULL;
	CHECK(logger_create(&cfg, &next) == -EBUSY && !next);
	CHECK(logger_default_flush() == -ENODEV);
	atomic_store(&released, 1);
	CHECK(pthread_join(t, NULL) == 0 && destroy_result == 0 && !owner);
	CHECK(logger_create(&cfg, &next) == 0);
	CHECK(logger_default_set(NULL, next) == 0);
	LOG_INFO("new");
	CHECK(logger_default_flush() == 0);
	CHECK(logger_default_set(next, NULL) == 0);
	CHECK(logger_destroy(&next) == 0);
	CHECK(file_contains("global.log", "old") &&
	      file_contains("global.log", "new"));
	return 0;
}
