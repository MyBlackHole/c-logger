#define _GNU_SOURCE
#include "logger.h"
#include "global_support.h"
#include "default_fixture.h"
static logger_t *owner;
#include <stdarg.h>

/* 取消探针仅用测试原子变量暂停，绝不用自身可能因取消而被遗留的测试互斥锁。
 * 真实库锁与生命周期保护是被测资源。检查掩码以及取消后的复用。
 */
enum point { P_NONE, P_WRITE, P_REOPEN, P_QUEUE, P_READER, P_CONTROL };
static _Atomic int at_point, release_point, seen_disabled, stage;
static _Atomic int hold_format, format_entered, release_format, flush_waiting;
static _Atomic int cleanup_seen, operation_returned;
static _Thread_local int delay_reader, delay_controller, watch_cond;
static int cleanup_phase_error;
static const char *scenario;

int __real_logger_file_write(struct logger_file *f, const char *text, size_t n,
			     const struct timespec *ts, int sync);
int __real_logger_file_reopen(struct logger_file *f);
int __real_logger_queue_push(struct logger_queue *q,
			     const struct logger_message *m);
size_t __real_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *out,
				 size_t cap);
int __real_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu);
int __real_pthread_mutex_lock(pthread_mutex_t *mu);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *rw);

static void inspect_mask(void)
{
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	atomic_store(&seen_disabled, old == PTHREAD_CANCEL_DISABLE);
	CHECK(!pthread_setcancelstate(old, NULL));
}
static void rendezvous(enum point p)
{
	int expected = p;
	if (!atomic_compare_exchange_strong(&stage, &expected, P_NONE))
		return;
	inspect_mask();
	atomic_store(&at_point, 1);
	wait_flag(&release_point);
}
int __wrap_logger_file_write(struct logger_file *f, const char *text, size_t n,
			     const struct timespec *ts, int sync)
{
	rendezvous(P_WRITE);
	return __real_logger_file_write(f, text, n, ts, sync);
}
int __wrap_logger_file_reopen(struct logger_file *f)
{
	rendezvous(P_REOPEN);
	return __real_logger_file_reopen(f);
}
int __wrap_logger_queue_push(struct logger_queue *q,
			     const struct logger_message *m)
{
	rendezvous(P_QUEUE);
	return __real_logger_queue_push(q, m);
}
size_t __wrap_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *out,
				 size_t cap)
{
	if (atomic_exchange(&hold_format, 0)) {
		atomic_store(&format_entered, 1);
		wait_flag(&release_format);
	}
	return __real_logger_format_line(l, m, out, cap);
}
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (watch_cond) {
		watch_cond = 0;
		inspect_mask();
		atomic_store(&flush_waiting, 1);
	}
	return __real_pthread_cond_wait(cv, mu);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (delay_controller) {
		delay_controller = 0;
		rendezvous(P_CONTROL);
	}
	return __real_pthread_mutex_lock(mu);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (delay_reader) {
		delay_reader = 0;
		rendezvous(P_READER);
	}
	return __real_pthread_rwlock_rdlock(rw);
}
static void cleanup(void *unused)
{
	(void)unused;
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	logger_io_metrics_t io;
	memset(&io, 0xa5, sizeof(io));
	errno = EDOM;
	CHECK(logger_get_global_io_metrics(&io) == -cleanup_phase_error);
	CHECK(errno == EDOM);
	if (cleanup_phase_error)
		expect_query_sentinel(&io, sizeof(io));
	if (!cleanup_phase_error) {
		LOG_INFO("cancel-cleanup-can-log");
		CHECK(logger_default_flush() == 0);
	}
	atomic_store(&cleanup_seen, 1);
	/* 线程正在退出，应用清理中有意不重新启用。 */
}
static void *caller(void *unused)
{
	(void)unused;
	pthread_cleanup_push(cleanup, NULL);
	if (!strcmp(scenario, "bind") || !strcmp(scenario, "control-wait")) {
		delay_controller = 1;
		CHECK(logger_default_set(NULL, owner) == 0);
	} else if (!strcmp(scenario, "unbind")) {
		delay_controller = 1;
		CHECK(logger_default_set(owner, NULL) == 0);
	} else if (!strcmp(scenario, "reopen")) {
		CHECK(logger_default_reopen() == 0);
	} else if (!strcmp(scenario, "flush") ||
		   !strcmp(scenario, "flush-void")) {
		watch_cond = 1;
		if (!strcmp(scenario, "flush"))
			CHECK(logger_default_flush() == 0);
		else
			logger_default_flush();
	} else if (!strcmp(scenario, "reader")) {
		delay_reader = 1;
		CHECK(logger_default_flush() == 0);
	} else {
		LOG_INFO("cancelled-operation-record");
	}
	atomic_store(&operation_returned, 1);
	pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}
static void restore_policy(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	CHECK(!fixture_default_create(&cfg, &owner));
	LOG_INFO("caller-disabled");
	CHECK(!logger_default_flush());
	CHECK(!fixture_default_destroy(&owner));
	int previous;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
	CHECK(previous == PTHREAD_CANCEL_DISABLE);
	CHECK(!pthread_setcancelstate(old, NULL));
	CHECK(!fixture_default_create(&cfg, &owner));
	LOG_INFO("caller-enabled");
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
	CHECK(previous == PTHREAD_CANCEL_ENABLE);
	CHECK(!pthread_setcancelstate(previous, NULL));
	CHECK(!fixture_default_destroy(&owner));
}
int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	scenario = argv[1];
	fixture_reset();
	if (!strcmp(scenario, "restore-policy")) {
		restore_policy();
		return 0;
	}
	int no_initial = !strcmp(scenario, "bind") ||
			 !strcmp(scenario, "control-wait");
	int async = !strcmp(scenario, "queue") || !strcmp(scenario, "flush") ||
		    !strcmp(scenario, "flush-void");
	logger_config_t cfg = config_for("old.log", async);
	CHECK(logger_create(&cfg, &owner) == 0);
	if (!no_initial)
		CHECK(logger_default_set(NULL, owner) == 0);
	cleanup_phase_error = !strcmp(scenario, "unbind") ? ENODEV : 0;
	if (no_initial || !strcmp(scenario, "unbind"))
		atomic_store(&stage, P_CONTROL);
	else if (!strcmp(scenario, "reopen"))
		atomic_store(&stage, P_REOPEN);
	else if (!strcmp(scenario, "queue"))
		atomic_store(&stage, P_QUEUE);
	else if (!strcmp(scenario, "reader"))
		atomic_store(&stage, P_READER);
	else if (!strcmp(scenario, "flush") ||
		 !strcmp(scenario, "flush-void")) {
		atomic_store(&hold_format, 1);
		LOG_INFO("worker-must-complete-before-cancel");
		wait_flag(&format_entered);
	} else {
		CHECK(!strcmp(scenario, "write"));
		atomic_store(&stage, P_WRITE);
	}
	pthread_t thread;
	CHECK(!pthread_create(&thread, NULL, caller, NULL));
	if (!strcmp(scenario, "flush") || !strcmp(scenario, "flush-void"))
		wait_flag(&flush_waiting);
	else
		wait_flag(&at_point);
	CHECK(atomic_load(&seen_disabled));
	CHECK(!pthread_cancel(thread));
	for (int i = 0; i < 20; ++i)
		nap_ms();
	CHECK(!atomic_load(&cleanup_seen)); /* 取消不得遗留已持有资源 */
	atomic_store(&release_point, 1);
	atomic_store(&release_format, 1);
	void *ret;
	CHECK(!pthread_join(thread, &ret));
	CHECK(ret == PTHREAD_CANCELED && atomic_load(&cleanup_seen));
	if (!cleanup_phase_error) {
		LOG_INFO("main-after-cancel");
		CHECK(!logger_default_flush());
		CHECK(!fixture_default_destroy(&owner));
		const char *path = "old.log";
		CHECK(file_contains(path, "cancel-cleanup-can-log"));
		CHECK(file_contains(path, "main-after-cancel"));
		if (!strcmp(scenario, "write") || !strcmp(scenario, "queue"))
			CHECK(file_contains(path,
					    "cancelled-operation-record"));
	} else {
		CHECK(logger_default_set(NULL, NULL) == 0);
		CHECK(logger_destroy(&owner) == 0);
	}
	CHECK(!logger_process_object_count());
	/* 没有遗留的全局控制锁、读锁或实例锁；另一代次可以工作。 */
	CHECK(!fixture_default_create(&cfg, &owner));
	LOG_INFO("next-generation-after-cancel");
	CHECK(!logger_default_flush() && !fixture_default_destroy(&owner));
	puts("global cancellation deferral and post-cancel reuse passed");
	return 0;
}
