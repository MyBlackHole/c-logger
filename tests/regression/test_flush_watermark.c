#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include "support.h"

#define WAITERS 4
static logger_t *instance;
static _Atomic int first_entered, first_release, second_entered, second_release;
static _Atomic int waiting, flushed;
static _Thread_local int flushing, counted;
static int results[WAITERS];
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	if (!strcmp(m->text, "before-watermark")) {
		atomic_store(&first_entered, 1);
		wait_flag(&first_release);
	} else if (!strcmp(m->text, "after-watermark")) {
		atomic_store(&second_entered, 1);
		wait_flag(&second_release);
	}
	return __real_logger_format_line(l, m, out, cap);
}
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (flushing && !counted && cv == &instance->progress_cv) {
		counted = 1;
		atomic_fetch_add(&waiting, 1);
	}
	return __real_pthread_cond_wait(cv, mu);
}
static void *flush_thread(void *p)
{
	int *result = p;
	flushing = 1;
	*result = logger_flush_instance_status(instance);
	flushing = 0;
	atomic_fetch_add(&flushed, 1);
	return NULL;
}
int main(void)
{
	char dir[] = "/tmp/logger-watermark-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	instance = logger_create(&c);
	CHECK(instance);
	LOGGER_INFO(instance, "barrier", "before-watermark");
	wait_flag(&first_entered);
	pthread_t threads[WAITERS];
	for (int i = 0; i < WAITERS; ++i)
		CHECK(pthread_create(&threads[i], NULL, flush_thread,
				     &results[i]) == 0);
	for (int i = 0; i < 5000 && atomic_load(&waiting) != WAITERS; ++i)
		nap_ms();
	CHECK(atomic_load(&waiting) == WAITERS);
	/* Each flush already captured target=1. Its progress condition is not
     * "queue empty", and later logging must not move its target. */
	LOGGER_INFO(instance, "barrier", "after-watermark");
	atomic_store(&first_release, 1);
	wait_flag(&second_entered);
	for (int i = 0; i < 5000 && atomic_load(&flushed) != WAITERS; ++i)
		nap_ms();
	CHECK(atomic_load(&flushed) == WAITERS);
	for (int i = 0; i < WAITERS; ++i) {
		CHECK(pthread_join(threads[i], NULL) == 0);
		CHECK(results[i] == 0);
	}
	logger_io_metrics_t io;
	logger_get_io_metrics(instance, &io);
	CHECK(io.async_completed == 1 && io.emitted_records == 1 &&
	      io.failed_records == 0);
	CHECK(file_contains("out.log", "before-watermark"));
	CHECK(!file_contains("out.log", "after-watermark"));
	atomic_store(&second_release, 1);
	CHECK(logger_flush_instance_status(instance) == 0);
	logger_get_io_metrics(instance, &io);
	CHECK(io.async_completed == 2 && io.emitted_records == 2 &&
	      io.first_error == 0);
	logger_destroy(instance);
	leave_temp(dir);
	puts("four flush waiters complete at their captured watermark despite later output");
	return 0;
}
