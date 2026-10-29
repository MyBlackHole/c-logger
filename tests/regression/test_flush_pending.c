#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include "support.h"

static _Atomic int formatting, release_format, flush_waiting, premature_fsync;
static _Thread_local int in_flush;
static off_t size_after_flush;
size_t __real_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *out,
				 size_t cap);
int __real_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu);
int __real_fsync(int fd);
size_t __wrap_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *out,
				 size_t cap)
{
	atomic_store(&formatting, 1);
	wait_flag(&release_format);
	return __real_logger_format_line(l, m, out, cap);
}
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (in_flush)
		atomic_store(&flush_waiting, 1);
	return __real_pthread_cond_wait(cv, mu);
}
int __wrap_fsync(int fd)
{
	if (in_flush && !atomic_load(&release_format))
		atomic_store(&premature_fsync, 1);
	return __real_fsync(fd);
}
static void *flusher(void *p)
{
	in_flush = 1;
	CHECK(logger_flush(p) == 0); /* 新接口仍须等待出队后尚未输出的记录。 */
	size_after_flush = file_size("out.log");
	in_flush = 0;
	return NULL;
}
int main(void)
{
	CHECK(logger_process_init() == 0);
	char dir[] = "/tmp/logger-flush-pending-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 8;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = NULL;
	CHECK(logger_create(&c, &l) == 0);
	CHECK(l != NULL);
	logger_log(l, LOGGER_INFO, "flush", __FILE__, __LINE__, __func__,
		   "pending-format");
	/* 避免将基线测试与其独立的唤醒丢失缺陷混为一谈。 */
	pthread_mutex_lock(&l->q.wait_mu);
	pthread_cond_signal(&l->q.wait_cv);
	pthread_mutex_unlock(&l->q.wait_mu);
	wait_flag(&formatting);
	pthread_t t;
	CHECK(pthread_create(&t, NULL, flusher, l) == 0);
	int synchronized = 0;
	for (int i = 0; i < 5000; ++i) {
		if (atomic_load(&flush_waiting) ||
		    atomic_load(&premature_fsync)) {
			synchronized = 1;
			break;
		}
		nap_ms();
	}
	CHECK(synchronized);
	int premature = atomic_load(&premature_fsync);
	CHECK(file_size("out.log") == 0);
	atomic_store(&release_format, 1);
	CHECK(pthread_join(t, NULL) == 0);
	logger_destroy(&l);
	CHECK(!premature);
	CHECK(size_after_flush > 0);
	CHECK(file_contains("out.log", "pending-format"));
	leave_temp(dir);
	puts("flush waits through dequeued-but-not-formatted output");
	return 0;
}
