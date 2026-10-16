#define _GNU_SOURCE
#include "global_support.h"
#include "logger_runtime.h"

/* 默认槽只借用宿主实例。以真实锁/后端窗口验证所有便利入口的代次与准入。 */
static _Thread_local int delay_read, count_reads;
static _Atomic int delayed, release_delayed, entered, release_backend;
static _Atomic int writer_waiting, reads, pause_write;
static logger_t *old_owner, *new_owner;
static int setter_result;

int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_logger_file_write(logger_file_t *, const char *, size_t,
			     const struct timespec *, int);
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (count_reads)
		atomic_fetch_add(&reads, 1);
	if (delay_read) {
		delay_read = 0;
		atomic_store(&delayed, 1);
		wait_flag(&release_delayed);
	}
	return __real_pthread_rwlock_rdlock(rw);
}
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	if (rw == &logger_runtime_current()->global_lifetime_lock)
		atomic_store(&writer_waiting, 1);
	return __real_pthread_rwlock_wrlock(rw);
}
int __wrap_logger_file_write(logger_file_t *f, const char *s, size_t n,
			     const struct timespec *ts, int sync)
{
	if (atomic_exchange(&pause_write, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	return __real_logger_file_write(f, s, n, ts, sync);
}
static void all_rejected(int error)
{
	for (int i = 0; i < OP_COUNT; ++i) {
		call_result_t r = { .op = (enum operation)i };
		invoke(&r);
		expect_rejection(&r, error);
	}
}
static void make_owners(void)
{
	logger_config_t a = config_for("old.log", 0);
	logger_config_t b = config_for("new.log", 0);
	CHECK(logger_create(&a, &old_owner) == 0);
	CHECK(logger_create(&b, &new_owner) == 0);
}
static void finish_owners(logger_t *bound)
{
	CHECK(logger_default_set(bound, NULL) == 0);
	CHECK(logger_destroy(&old_owner) == 0);
	CHECK(logger_destroy(&new_owner) == 0);
	CHECK(logger_process_object_count() == 0);
	all_rejected(ENODEV);
}
static void *delayed_call(void *r)
{
	delay_read = 1;
	invoke(r);
	return NULL;
}
static void generation(enum operation op)
{
	make_owners();
	CHECK(logger_default_set(NULL, old_owner) == 0);
	LOG_INFO("old-base");
	call_result_t r = { .op = op };
	pthread_t reader;
	CHECK(pthread_create(&reader, NULL, delayed_call, &r) == 0);
	wait_flag(&delayed);
	CHECK(logger_default_set(old_owner, new_owner) == 0);
	CHECK(logger_destroy(&old_owner) == 0);
	LOG_INFO("new-base");
	atomic_store(&release_delayed, 1);
	CHECK(pthread_join(reader, NULL) == 0);
	expect_rejection(&r, ESHUTDOWN);
	/* 延迟级别设置、重开、刷新和写入均不能落到新代次。 */
	CHECK(logger_get_state(new_owner) == LOGGER_STATE_RUNNING);
	LOG_INFO("new-still-visible");
	finish_owners(new_owner);
	CHECK(file_lines("old.log") == 1 && file_lines("new.log") == 2);
	CHECK(!file_contains("new.log", "delayed-record"));
}
static void *writer(void *unused)
{
	(void)unused;
	LOG_INFO("pinned-before-replace");
	return NULL;
}
static void *setter(void *unused)
{
	(void)unused;
	setter_result = logger_default_set(old_owner, new_owner);
	return NULL;
}
static void *flood(void *unused)
{
	(void)unused;
	count_reads = 1;
	for (int i = 0; i < 32; ++i)
		all_rejected(EAGAIN);
	return NULL;
}
static void closed_gate(void)
{
	make_owners();
	CHECK(logger_default_set(NULL, old_owner) == 0);
	atomic_store(&writer_waiting, 0);
	atomic_store(&pause_write, 1);
	pthread_t w, s, readers[8];
	CHECK(pthread_create(&w, NULL, writer, NULL) == 0);
	wait_flag(&entered);
	CHECK(pthread_create(&s, NULL, setter, NULL) == 0);
	wait_flag(&writer_waiting);
	CHECK(logger_destroy(&old_owner) == -EBUSY && old_owner != NULL);
	for (int i = 0; i < 8; ++i)
		CHECK(pthread_create(&readers[i], NULL, flood, NULL) == 0);
	for (int i = 0; i < 8; ++i)
		CHECK(pthread_join(readers[i], NULL) == 0);
	CHECK(atomic_load(&reads) == 0);
	atomic_store(&release_backend, 1);
	CHECK(pthread_join(w, NULL) == 0 && pthread_join(s, NULL) == 0);
	CHECK(setter_result == 0);
	LOGGER_INFO(old_owner, "host", "old-owner-still-usable");
	LOG_INFO("new-default");
	finish_owners(new_owner);
	CHECK(file_lines("old.log") == 2 && file_lines("new.log") == 1);
}
int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc >= 2);
	fixture_reset();
	if (!strcmp(argv[1], "generation")) {
		CHECK(argc == 3);
		generation(parse_operation(argv[2]));
	} else if (!strcmp(argv[1], "closed-gate")) {
		closed_gate();
	} else {
		CHECK(!strcmp(argv[1], "contract"));
		all_rejected(ENODEV);
		make_owners();
		errno = EDOM;
		CHECK(logger_default_set(NULL, old_owner) == 0 &&
		      errno == EDOM);
		CHECK(logger_default_set(NULL, new_owner) == -ESTALE);
		CHECK(logger_default_set(old_owner, old_owner) == 0);
		CHECK(logger_destroy(&old_owner) == -EBUSY && old_owner);
		LOG_INFO("contract-record");
		finish_owners(old_owner);
		CHECK(file_lines("old.log") == 1);
	}
	puts("GLOBAL_BORROW_LIFECYCLE_PASS");
	return 0;
}
