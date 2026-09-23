#include "logger.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int go = 1;
static void *producer(void *x)
{
	(void)x;
	unsigned long i = 0;
	while (atomic_load(&go)) {
		LOG_INFO("race %lu", i++);
	}
	return 0;
}
int main(void)
{
	unlink("./lifecycle.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./lifecycle.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 4096;
	if (logger_init(&c))
		return 1;
	pthread_t t[8];
	for (int i = 0; i < 8; i++)
		if (pthread_create(&t[i], 0, producer, 0))
			return 2;
	usleep(20000);
	logger_shutdown(); /* races intentionally with producers */
	atomic_store(&go, 0);
	for (int i = 0; i < 8; i++)
		pthread_join(t[i], 0);
	/* post-shutdown global logging must be harmless */
	LOG_INFO("ignored after shutdown");
	return 0;
}
