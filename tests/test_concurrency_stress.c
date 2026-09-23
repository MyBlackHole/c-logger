#include "logger.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
static logger_t *g;
static _Atomic int run = 1;
static void *prod(void *x)
{
	long id = (long)x;
	unsigned long n = 0;
	while (atomic_load(&run))
		logger_log(g, LOGGER_INFO, "stress", __FILE__, __LINE__,
			   __func__, "p=%ld n=%lu", id, n++);
	return 0;
}
static void *ctl(void *x)
{
	(void)x;
	logger_metrics_t m;
	while (atomic_load(&run)) {
		logger_set_instance_level(g, LOGGER_DEBUG);
		logger_get_metrics(g, &m);
		logger_flush_instance(g);
		logger_set_instance_level(g, LOGGER_INFO);
	}
	return 0;
}
int main(void)
{
	unlink("./stress.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./stress.log";
	c.queue_capacity = 16384;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	g = logger_create(&c);
	if (!g)
		return 1;
	pthread_t p[12], c1, c2;
	for (long i = 0; i < 12; i++)
		if (pthread_create(&p[i], 0, prod, (void *)i))
			return 2;
	if (pthread_create(&c1, 0, ctl, 0) || pthread_create(&c2, 0, ctl, 0))
		return 3;
	usleep(100000);
	atomic_store(&run, 0);
	for (int i = 0; i < 12; i++)
		pthread_join(p[i], 0);
	pthread_join(c1, 0);
	pthread_join(c2, 0);
	logger_destroy(g);
	return 0;
}
