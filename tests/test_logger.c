#define LOG_MODULE "test"
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
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./app.log";
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 128 * 1024;
	c.rotation.retention_days = 30;
	c.queue_capacity = 512;
	if (logger_init(&c)) {
		perror("logger_init");
		return 1;
	}
	pthread_t t[4];
	for (long i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, run, (void *)i);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	LOG_ERROR("example high-severity message");
	printf("dropped=%llu\n", (unsigned long long)logger_global_dropped());
	logger_shutdown();
	return 0;
}
