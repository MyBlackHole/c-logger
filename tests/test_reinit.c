#include "logger.h"
#include "regression/support.h"

static logger_t *owner;
static _Atomic int won;
static _Atomic int rejected;
static pthread_barrier_t barrier;
static void *bind_once(void *unused)
{
	(void)unused;
	int rc = pthread_barrier_wait(&barrier);
	CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
	rc = logger_default_set(NULL, owner);
	if (!rc)
		atomic_fetch_add(&won, 1);
	else {
		CHECK(rc == -ESTALE || rc == -EAGAIN);
		atomic_fetch_add(&rejected, 1);
	}
	return NULL;
}
int main(void)
{
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "reinit.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	CHECK(logger_default_set(NULL, owner) == -ESTALE);
	CHECK(logger_default_set(owner, owner) == 0);
	CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(pthread_barrier_init(&barrier, NULL, 8) == 0);
	pthread_t threads[8];
	for (int i = 0; i < 8; ++i)
		CHECK(pthread_create(&threads[i], NULL, bind_once, NULL) == 0);
	for (int i = 0; i < 8; ++i)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(atomic_load(&won) == 1 && atomic_load(&rejected) == 7);
	CHECK(pthread_barrier_destroy(&barrier) == 0);
	CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(logger_destroy(&owner) == 0 && !owner);
	return 0;
}
