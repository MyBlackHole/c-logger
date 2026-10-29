#define LOG_MODULE "test"
#include <errno.h>
#include "regression/default_fixture.h"
static logger_t *default_owner;
#include "logger.h"
#include <pthread.h>
#include <stdio.h>

static void *run(void *p)
{
	long id = (long)p;

	for (int i = 0; i < 1000; i++)
		LOG_INFO("worker=%ld seq=%d", id, i);
	return NULL;
}

int main(void)
{
	if (logger_process_init() != 0)
		return 1;
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./app.log";
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 128 * 1024;
	c.rotation.retention_days = 30;
	c.queue_capacity = 512;
	if (fixture_default_create(&c, &default_owner)) {
		fputs("DEFAULT_BIND_FAILED\n", stderr);
		return 1;
	}
	pthread_t t[4];

	for (long i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, run, (void *)i);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	LOG_ERROR("example high-severity message");
	uint64_t dropped;

	if (logger_global_dropped(&dropped)) {
		fixture_default_destroy(&default_owner);
		return 2;
	}
	printf("dropped=%llu\n", (unsigned long long)dropped);
	fixture_default_destroy(&default_owner);
	return 0;
}
