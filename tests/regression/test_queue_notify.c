#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include "support.h"

/* No library hook: wrap the real pthread wait entry at link time. */
static _Atomic int in_gap, release_wait, intercepted;
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (!atomic_exchange(&intercepted, 1)) {
		atomic_store(&in_gap, 1);
		wait_flag(
			&release_wait); /* worker still holds the predicate mutex */
	}
	return __real_pthread_cond_wait(cv, mu);
}
static void *producer(void *p)
{
	logger_log(p, LOGGER_INFO, "notify", __FILE__, __LINE__, __func__,
		   "last-before-idle");
	return NULL;
}
int main(void)
{
	char dir[] = "/tmp/logger-notify-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	CHECK(l);
	wait_flag(&in_gap);
	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, producer, l) == 0);
	/* Wait until publication, NOT producer return: the fixed notification may
     * legitimately block on wait_mu until consumer actually starts waiting. */
	int published = 0;
	for (int i = 0; i < 5000; ++i) {
		if (atomic_load_explicit(&l->q.slots[0].seq,
					 memory_order_acquire) == 1) {
			published = 1;
			break;
		}
		nap_ms();
	}
	CHECK(published);
	atomic_store(&release_wait, 1);
	CHECK(pthread_join(thread, NULL) == 0);
	int output_without_rescue = 0;
	for (int i = 0; i < 1000; ++i) {
		if (file_size("out.log") > 0) {
			output_without_rescue = 1;
			break;
		}
		nap_ms();
	}
	/* Record the result BEFORE shutdown sends a rescue notification. */
	logger_destroy(l);
	CHECK(file_contains("out.log", "last-before-idle"));
	CHECK(output_without_rescue);
	leave_temp(dir);
	puts("queue notification survives publication in the wait-entry gap");
	return 0;
}
