#define _GNU_SOURCE
#include "global_support.h"
#include "logger_lockdep.h"
#include <stdarg.h>
#include <stdint.h>
#include <sys/wait.h>

/* 库中没有钩子。GNU ld 包装函数控制精确的加锁前与后端内部边界；
 * 可执行文件拥有每次暂停与失败。
 */
static _Thread_local int delay_read, delay_control, count_reads, note_writer;
static _Thread_local int fail_read, fail_control, fail_writer;
static _Atomic int delayed, release_delayed, entered, release_backend;
static _Atomic int writer_waiting, read_count, pause_create, pause_destroy,
	pause_write;
static _Atomic int fail_create, create_count, fail_dispose;
/* R7 只追踪 logger_t 容器的实际 free，不把 destroy 返回码当作释放证明。 */
static _Atomic uintptr_t tracked_instance;
static _Atomic unsigned tracked_frees, tracked_destroy_calls;
static _Atomic int reject_destroy_without_receipt;
static _Atomic int recurse_create, recurse_destroy, recurse_write,
	recurse_reopen, recurse_bootstrap;
static _Atomic int recursion_checked, pause_bootstrap, fail_sync;
static int admission_only;
/* 链接时失败不调用真实释放：锁所有权有意保持不确定。
 * 每个失效场景在自己的进程中运行。
 */
static _Thread_local int fail_rw_unlock, fail_control_unlock;
static _Thread_local int fail_join, fail_cond_destroy;
static _Thread_local int handoff_reader, handoff_writer;
static _Atomic int handoff_reader_at_unlock, handoff_writer_at_lock;
static _Atomic int handoff_release_reader, handoff_release_writer;
static _Atomic int handoff_fail_writer;
#if LOGGER_ENABLE_LOCKDEP
/* 仅使用回归支持库的真实锁类断言作为暂停边界。
 * 生产 liblogger.a 中该断言为空内联，不增加任何生产测试钩子。
 */
static _Thread_local int r7_pause_before_publish;
static _Atomic int r7_publish_assert_reached, r7_resume_publish;
static _Atomic int r7_pause_after_real_unlock, r7_reader_released;
static _Atomic int r7_report_unlock_error, r7_reader_error;
#endif
static _Thread_local int capture_control;
static _Thread_local pthread_t rejected_worker;
static _Thread_local int saw_rejected_join;
static pthread_mutex_t *control_mutex;
#if LOGGER_ENABLE_LOCKDEP
void __real_logger_lockdep_assert_held(logger_lock_class_t, const void *);
void __wrap_logger_lockdep_assert_held(logger_lock_class_t class_id,
				       const void *lock)
{
	if (r7_pause_before_publish &&
	    class_id == LOGGER_LOCK_GLOBAL_LIFETIME) {
		r7_pause_before_publish = 0;
		atomic_store(&r7_publish_assert_reached, 1);
		wait_flag(&r7_resume_publish);
	}
	__real_logger_lockdep_assert_held(class_id, lock);
}
#endif

int __real_fsync(int);
int __wrap_fsync(int fd)
{
	if (atomic_exchange(&fail_sync, 0)) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *);
int __real_pthread_join(pthread_t, void **);
int __real_pthread_cond_destroy(pthread_cond_t *);
int __real_logger_create(const logger_config_t *, logger_t **);
int __real_logger_destroy(logger_t **);
int __real_logger_runtime_free(void *);

int __wrap_logger_runtime_free(void *ptr)
{
	uintptr_t candidate = atomic_load(&tracked_instance);
	if (ptr && (uintptr_t)ptr == candidate) {
		atomic_fetch_add(&tracked_frees, 1);
		atomic_store(&tracked_instance, 0);
	}
	return __real_logger_runtime_free(ptr);
}
int __real_logger_file_write(logger_file_t *, const char *, size_t,
			     const struct timespec *, int);
int __real_logger_file_reopen(logger_file_t *);

static void verify_reentry(void)
{
	logger_config_t cfg = config_for("side.log", 0);
	CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
	CHECK(stop_status() == -1 && errno == EDEADLK);
	for (int i = 0; i < OP_COUNT; ++i) {
		call_result_t r = { .op = (enum operation)i };
		invoke(&r);
		expect_rejection(&r, EDEADLK);
	}
	CHECK(access("side.log", F_OK) == -1);
	atomic_fetch_add(&recursion_checked, 1);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (capture_control) {
		control_mutex = mu;
		capture_control = 0;
	}
	if (delay_control) {
		delay_control = 0;
		atomic_store(&delayed, 1);
		wait_flag(&release_delayed);
	}
	if (fail_control) {
		int rc = fail_control;
		fail_control = 0;
		return rc;
	}
	return __real_pthread_mutex_lock(mu);
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (mu == control_mutex && fail_control_unlock) {
		int error = fail_control_unlock;
		fail_control_unlock = 0;
		return error; /* 所有者仍持有真实互斥锁 */
	}
	return __real_pthread_mutex_unlock(mu);
}
int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
	if (handoff_reader) {
		handoff_reader = 0;
		/* 真实释放后的合成错误：看到非零返回的调用者
		 * 仍必须视所有权为未知，而另一条真实 pthread 线程可以安全获取锁。
		 */
		atomic_store(&handoff_reader_at_unlock, 1);
		wait_flag(&handoff_release_reader);
		CHECK(__real_pthread_rwlock_unlock(rw) == 0);
#if LOGGER_ENABLE_LOCKDEP
		/* 读者真实释放生命周期锁后，先暂停错误的报告。
		 * 让控制器已经越过检查 poison 的位置，再触发 BROKEN。
		 */
		if (atomic_load(&r7_pause_after_real_unlock)) {
			atomic_store(&r7_reader_released, 1);
			wait_flag(&r7_report_unlock_error);
		}
#endif
		return EIO;
	}
	if (fail_rw_unlock) {
		int error = fail_rw_unlock;
		fail_rw_unlock = 0;
		return error; /* 不实际解锁：所有权不确定 */
	}
	return __real_pthread_rwlock_unlock(rw);
}
int __wrap_pthread_join(pthread_t thread, void **result)
{
	if (fail_join) {
		int error = fail_join;
		fail_join = 0;
		rejected_worker = thread;
		saw_rejected_join = 1;
		return error; /* 生产线程回收失败：禁止最终 free */
	}
	return __real_pthread_join(thread, result);
}
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (fail_cond_destroy) {
		int error = fail_cond_destroy;
		fail_cond_destroy = 0;
		return error;
	}
	return __real_pthread_cond_destroy(cv);
}
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (count_reads)
		atomic_fetch_add(&read_count, 1);
	if (delay_read) {
		delay_read = 0;
		atomic_store(&delayed, 1);
		wait_flag(&release_delayed);
	}
	if (fail_read) {
		int rc = fail_read;
		fail_read = 0;
		return rc;
	}
	return __real_pthread_rwlock_rdlock(rw);
}
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
	if (handoff_writer) {
		handoff_writer = 0;
		atomic_store(&handoff_writer_at_lock, 1);
		wait_flag(&handoff_release_writer);
	}
	if (note_writer) {
		note_writer = 0;
		atomic_store(&writer_waiting, 1);
	}
	if (fail_writer) {
		int rc = fail_writer;
		fail_writer = 0;
		return rc;
	}
	return __real_pthread_rwlock_wrlock(rw);
}
int __wrap_logger_create(const logger_config_t *cfg, logger_t **out)
{
	atomic_fetch_add(&create_count, 1);
	if (atomic_exchange(&pause_create, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_create, 0))
		verify_reentry();
	if (atomic_exchange(&fail_create, 0)) {
		return -ENOSPC;
	}
	int rc = __real_logger_create(cfg, out);
	if (!rc)
		atomic_store(&tracked_instance, (uintptr_t)*out);
	return rc;
}
int __wrap_logger_destroy(logger_t **owner)
{
	atomic_fetch_add_explicit(&tracked_destroy_calls, 1,
				  memory_order_relaxed);
	/* 受控故障发生在最终释放前，不触碰真实同步对象。 */
	if (atomic_exchange(&reject_destroy_without_receipt, 0)) {
		return -EIO;
	}
	if (atomic_exchange(&pause_destroy, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_destroy, 0))
		verify_reentry();
	int rc = __real_logger_destroy(owner);
	if (atomic_exchange(&fail_dispose, 0)) {
		return -EIO;
	}
	return rc;
}
int __wrap_logger_file_write(logger_file_t *f, const char *text, size_t n,
			     const struct timespec *ts, int sync)
{
	if (atomic_exchange(&pause_write, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_write, 0))
		verify_reentry();
	return __real_logger_file_write(f, text, n, ts, sync);
}
int __wrap_logger_file_reopen(logger_file_t *f)
{
	if (atomic_exchange(&recurse_reopen, 0))
		verify_reentry();
	return __real_logger_file_reopen(f);
}
int __real_logger_stderr_writev_all(struct iovec *, int);
int __wrap_logger_stderr_writev_all(struct iovec *vec, int count)
{
	if (atomic_exchange(&pause_bootstrap, 0)) {
		atomic_store(&entered, 1);
		wait_flag(&release_backend);
	}
	if (atomic_exchange(&recurse_bootstrap, 0))
		verify_reentry();
	return __real_logger_stderr_writev_all(vec, count);
}

static void *delayed_call(void *p)
{
	delay_read = 1;
	invoke(p);
	return NULL;
}
static void *shutdown_call(void *p)
{
	call_result_t *r = p;
	note_writer = 1;
	r->rc = stop_status();
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}
static void *delayed_shutdown(void *p)
{
	delay_control = 1;
	return shutdown_call(p);
}
static void *init_call(void *p)
{
	call_result_t *r = p;
	logger_config_t cfg = config_for("old.log", 0);
	r->rc = logger_init(&cfg);
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}
static void *write_call(void *p)
{
	call_result_t *r = p;
	LOG_INFO("pinned-before-stop");
	atomic_store(&r->done, 1);
	return NULL;
}

static void generation(enum operation op, int bootstrap)
{
	logger_config_t a = config_for("old.log", 0),
			b = config_for("new.log", 0);
	if (!bootstrap) {
		CHECK(logger_init(&a) == 0);
		LOG_INFO("old-base");
	}
	pthread_t t;
	call_result_t r = { .op = op };
	CHECK(!pthread_create(&t, NULL, delayed_call, &r));
	wait_flag(&delayed); /* 票据已采样，尚未获取读锁 */
	if (!bootstrap)
		CHECK(stop_status() == 0);
	CHECK(logger_init(&b) == 0);
	LOG_INFO("new-base");
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(t, NULL));
	expect_rejection(&r, ESHUTDOWN);
	LOG_INFO("new-after-stale");
	CHECK(logger_flush_status() == 0);
	CHECK(stop_status() == 0);
	CHECK(file_lines("new.log") == 2 &&
	      file_contains("new.log", "new-after-stale"));
	CHECK(!file_contains("new.log", "delayed-record"));
	if (!bootstrap)
		CHECK(file_lines("old.log") == 1);
}
static void shutdown_generation(void)
{
	logger_config_t a = config_for("old.log", 0),
			b = config_for("new.log", 0);
	CHECK(!logger_init(&a));
	pthread_t t;
	call_result_t r = { 0 };
	CHECK(!pthread_create(&t, NULL, delayed_shutdown, &r));
	wait_flag(&delayed); /* 关闭已采样旧代次，未持控制锁 */
	CHECK(!stop_status());
	CHECK(!logger_init(&b));
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(t, NULL));
	CHECK(r.rc == -1 && r.error == ESHUTDOWN);
	LOG_INFO("new-not-stopped");
	CHECK(!logger_flush_status() && !stop_status());
	CHECK(file_contains("new.log", "new-not-stopped"));
}
static void all_rejected(int error)
{
	for (int i = 0; i < OP_COUNT; ++i) {
		call_result_t r = { .op = (enum operation)i };
		/* 验证被拒绝的指标查询用零覆盖调用者旧数据。 */
		memset(&r.metrics, 0xa5, sizeof(r.metrics));
		memset(&r.io, 0xa5, sizeof(r.io));
		memset(&r.diagnostics, 0xa5, sizeof(r.diagnostics));
		memset(&r.file_metrics, 0xa5, sizeof(r.file_metrics));
		invoke(&r);
		expect_rejection(&r, error);
		if (r.op == OP_FILE_METRICS)
			CHECK(((const unsigned char *)&r.file_metrics)[0] == 0xa5);
	}
}
static void start_gate(void)
{
	pthread_t t;
	call_result_t r = { 0 };
	atomic_store(&pause_create, 1);
	CHECK(!pthread_create(&t, NULL, init_call, &r));
	wait_flag(&entered); /* 候选对象存在之前 */
	count_reads = 1;
	all_rejected(EAGAIN);
	count_reads = 0;
	CHECK(!atomic_load(&read_count));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(t, NULL) && r.rc == 0);
	LOG_INFO("published-only-now");
	CHECK(!stop_status() && file_lines("old.log") == 1);
}
static void *flood_closed(void *p)
{
	(void)p;
	count_reads = 1;
	for (int i = 0; i < 100; ++i) {
		if (admission_only) {
			logger_metrics_t metrics;
			logger_get_global_metrics(&metrics);
			logger_diagnostics_t diagnostics;
			logger_get_global_diagnostics(&diagnostics);
			(void)logger_global_dropped();
			logger_set_level(LOGGER_INFO);
		} else
			all_rejected(ESHUTDOWN);
	}
	return NULL;
}
static void stop_gate(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pthread_t writer, stopper, readers[8];
	call_result_t w = { 0 }, s = { 0 };
	atomic_store(&pause_write, 1);
	CHECK(!pthread_create(&writer, NULL, write_call, &w));
	wait_flag(&entered); /* 现有读者位于后端 emit_mu 内 */
	CHECK(!pthread_create(&stopper, NULL, shutdown_call, &s));
	wait_flag(&writer_waiting); /* 准入已关闭，写锁即将等待 */
	for (int i = 0; i < 8; ++i)
		CHECK(!pthread_create(&readers[i], NULL, flood_closed, NULL));
	for (int i = 0; i < 8; ++i)
		CHECK(!pthread_join(readers[i], NULL));
	CHECK(!atomic_load(&read_count));
	CHECK(!atomic_load(&s.done));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(writer, NULL) && !pthread_join(stopper, NULL));
	CHECK(!s.rc && file_lines("old.log") == 1);
	CHECK(file_contains("old.log", "pinned-before-stop"));
}
static void bootstrap_stop(void)
{
	pthread_t w, s;
	call_result_t wr = { 0 }, sr = { 0 };
	atomic_store(&pause_bootstrap, 1);
	CHECK(!pthread_create(&w, NULL, write_call, &wr));
	wait_flag(&entered);
	CHECK(!pthread_create(&s, NULL, shutdown_call, &sr));
	wait_flag(&writer_waiting);
	for (int i = 0; i < 20; ++i)
		nap_ms();
	CHECK(!atomic_load(&sr.done));
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(w, NULL) && !pthread_join(s, NULL));
	CHECK(!sr.rc);
	all_rejected(ESHUTDOWN);
}
static void during_start_stop(void)
{
	pthread_t init, stop;
	call_result_t i = { 0 }, s = { 0 };
	atomic_store(&pause_create, 1);
	CHECK(!pthread_create(&init, NULL, init_call, &i));
	wait_flag(&entered);
	CHECK(!pthread_create(&stop, NULL, delayed_shutdown, &s));
	wait_flag(&delayed);
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(init, NULL) && !i.rc);
	atomic_store(&release_delayed, 1);
	CHECK(!pthread_join(stop, NULL) && !s.rc);
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
	CHECK(logger_process_object_count() == 0);
}
static void double_shutdown(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pthread_t one, two;
	call_result_t a = { 0 }, b = { 0 };
	atomic_store(&pause_destroy, 1);
	CHECK(!pthread_create(&one, NULL, shutdown_call, &a));
	wait_flag(&entered);
	CHECK(!pthread_create(&two, NULL, delayed_shutdown, &b));
	wait_flag(&delayed);
	atomic_store(&release_delayed, 1);
	atomic_store(&release_backend, 1);
	CHECK(!pthread_join(one, NULL) && !pthread_join(two, NULL));
	CHECK(!a.rc && !b.rc && !logger_process_object_count());
}
static void recursion(const char *origin)
{
	logger_config_t cfg = config_for("old.log", 0);
	if (!strcmp(origin, "bootstrap")) {
		atomic_store(&recurse_bootstrap, 1);
		LOG_INFO("bootstrap");
		CHECK(atomic_load(&recursion_checked) == 1);
		return;
	}
	if (!strcmp(origin, "create"))
		atomic_store(&recurse_create, 1);
	CHECK(!logger_init(&cfg));
	if (!strcmp(origin, "write")) {
		atomic_store(&recurse_write, 1);
		LOG_INFO("outer-survived");
	} else if (!strcmp(origin, "reopen")) {
		atomic_store(&recurse_reopen, 1);
		CHECK(!logger_reopen());
	} else if (!strcmp(origin, "destroy"))
		atomic_store(&recurse_destroy, 1);
	CHECK(!stop_status());
	CHECK(atomic_load(&recursion_checked) == 1);
	CHECK(!logger_process_object_count());
}
static void acquisition_error(const char *which)
{
	logger_config_t cfg = config_for("old.log", 0);
	if (!strcmp(which, "control")) {
		fail_control = EBUSY;
		CHECK(logger_init(&cfg) == -1 && errno == EBUSY);
		CHECK(!atomic_load(&create_count));
	} else if (!strcmp(which, "publish")) {
		fail_writer = EDEADLK;
		CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
		CHECK(!logger_process_object_count());
	} else if (!strcmp(which, "create")) {
		atomic_store(&fail_create, 1);
		CHECK(logger_init(&cfg) == -1 && errno == ENOSPC);
		CHECK(!logger_process_object_count());
		CHECK(logger_flush_status() == -1 && errno == ENODEV);
	}
	CHECK(!logger_init(&cfg));
	if (!strcmp(which, "read")) {
		fail_read = EAGAIN;
		CHECK(logger_flush_status() == -1 && errno == EAGAIN);
	} else if (!strcmp(which, "stop")) {
		fail_writer = EDEADLK;
		CHECK(stop_status() == -1 && errno == EDEADLK);
	}
	LOG_INFO("after-error");
	CHECK(!logger_flush_status() && !stop_status());
	CHECK(file_lines("old.log") == 1 && !logger_process_object_count());
}
static void *verify_poisoned_controller(void *arg)
{
	int error = *(int *)arg;
	all_rejected(error);
	logger_config_t cfg = config_for("new.log", 0);
	CHECK(logger_init(&cfg) == -1 && errno == error);
	CHECK(stop_status() == -1 && errno == error);
	return NULL;
}

/* 读者到达释放点前已取得生命周期保护。控制方关闭准入，
 * 在获取写锁前暂停，然后读者返回合成的解锁错误。
 * 两种交错都必须保留 BROKEN：写锁成功不得撤销发布，
 * 写锁获取失败也不得回滚已失效的代次。
 */
static void *handoff_reader_thread(void *arg)
{
	call_result_t *r = arg;
	handoff_reader = 1;
	r->rc = logger_flush_status();
	r->error = errno;
	atomic_store(&r->done, 1);
	return NULL;
}

static void *handoff_shutdown_thread(void *arg)
{
	handoff_writer = 1;
	if (atomic_load(&handoff_fail_writer))
		fail_writer = EDEADLK;
	return shutdown_call(arg);
}

static void unlock_shutdown_handoff(int acquire_fails)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(logger_init(&cfg) == 0);
	atomic_store(&handoff_fail_writer, acquire_fails);

	pthread_t reader, controller;
	call_result_t a = { 0 }, b = { 0 };
	CHECK(!pthread_create(&reader, NULL, handoff_reader_thread, &a));
	wait_flag(&handoff_reader_at_unlock);
	/* 读者仍拥有锁；关闭必须先关闭准入。 */
	CHECK(!pthread_create(&controller, NULL, handoff_shutdown_thread,
				 &b));
	wait_flag(&handoff_writer_at_lock);

	/* 对于此受控错误，真实解锁成功。注入错误
	 * 只在读者已释放该生命周期保护后返回。
	 */
	atomic_store(&handoff_release_reader, 1);
	CHECK(!pthread_join(reader, NULL));
	CHECK(a.rc == -1 && a.error == EIO);

	atomic_store(&handoff_release_writer, 1);
	CHECK(!pthread_join(controller, NULL));
	if (acquire_fails)
		CHECK(b.rc == -1 && b.error == EDEADLK);
	else
		CHECK(b.rc == -1 && b.error == EIO);

	/* 即使写锁成功获取，读者造成的失效仍优先。
	 * 唯一存活实例仍由全局控制方拥有。
	 */
	CHECK(logger_process_object_count() == 1);
	all_rejected(EIO);
	logger_config_t replacement = config_for("new.log", 0);
	CHECK(logger_init(&replacement) == -1 && errno == EIO);
	CHECK(access("new.log", F_OK) == -1 && errno == ENOENT);
}

#if LOGGER_ENABLE_LOCKDEP
/* G_IDLE 引导写入合法获取生命周期读锁，供故障注入控制真正的读者。
 * 与模拟直接写 ticket 不同：错误通过真实 Global 读者解锁路径发布。
 */
static void *r7_bootstrap_reader(void *unused)
{
	(void)unused;
	handoff_reader = 1;
	errno = 0;
	LOG_INFO("r7-bootstrap-reader");
	atomic_store(&r7_reader_error, errno);
	return NULL;
}

static void *r7_publication_writer(void *arg)
{
	r7_pause_before_publish = 1;
	return init_call(arg);
}

static void r7_exact_publication_cas_failure(void)
{
	logger_config_t next = config_for("new.log", 0);
	pthread_t reader, publisher;
	call_result_t published = { 0 };

	/* 读者保持锁，直到初始化者创建候选并准备获取写锁。 */
	atomic_store(&r7_pause_after_real_unlock, 1);
	CHECK(!pthread_create(&reader, NULL, r7_bootstrap_reader, NULL));
	wait_flag(&handoff_reader_at_unlock);
	CHECK(!pthread_create(&publisher, NULL, r7_publication_writer,
			      &published));

	/* 真实读锁先释放，但读者尚未发布合成 EIO。
	 * 创建线程必须取得写锁并越过 global_lock_error() 首次检查。
	 */
	atomic_store(&handoff_release_reader, 1);
	wait_flag(&r7_reader_released);
	wait_flag(&r7_publish_assert_reached);
	CHECK(atomic_load(&create_count) == 1);
	CHECK(logger_process_object_count() == 1);
	CHECK(!atomic_load(&tracked_destroy_calls));
	CHECK(!atomic_load(&tracked_frees));

	/* 仅在初始化线程已通过当前代次检查后报告错误：
	 * 否则只验证其提前检测 BROKEN，根本不会触发发布 CAS 失败分支。
	 */
	atomic_store(&r7_report_unlock_error, 1);
	CHECK(!pthread_join(reader, NULL));
	CHECK(atomic_load(&r7_reader_error) == EIO);

	/* 现在才允许运行实际 G_STARTING -> G_RUNNING 强 CAS；
	 * 必须失败，撤销未发布指针并真正完成候选退役。
	 */
	atomic_store(&r7_resume_publish, 1);
	CHECK(!pthread_join(publisher, NULL));
	CHECK(published.rc == -1 && published.error == EIO);
	CHECK(atomic_load(&tracked_destroy_calls) == 1);
	CHECK(atomic_load(&tracked_frees) == 1);
	CHECK(!atomic_load(&tracked_instance));
	CHECK(!logger_process_object_count());

	all_rejected(EIO);
	CHECK(logger_init(&next) == -1 && errno == EIO);
	CHECK(access("new.log", F_OK) == -1 && errno == ENOENT);
}
#endif

/* 下面每项独立进程运行：失败的 lock/proof 路径不在测试中非法销毁。
 * 生产实现没有增加 hook，所有拒绝都作用于真实链接入口。
 */
static void r7_poisoned_state(void)
{
	int error = EIO;
	pthread_t verifier;

	CHECK(!pthread_create(&verifier, NULL, verify_poisoned_controller,
			      &error));
	CHECK(!pthread_join(verifier, NULL));
	CHECK(logger_process_object_count() == 1);
	CHECK(!atomic_load(&tracked_frees));
	CHECK(atomic_load(&tracked_instance) != 0);
}

static void r7_stage_contract(const char *scenario)
{
	logger_config_t cfg = config_for("old.log", 0);
	logger_config_t next = config_for("new.log", 0);

	if (!strcmp(scenario, "normal")) {
		CHECK(!logger_init(&cfg));
		CHECK(logger_process_object_count() == 1);
		CHECK(!atomic_load(&tracked_frees));
		LOG_INFO("r7-first-generation");
		CHECK(!logger_flush_status() && !stop_status());
		CHECK(logger_process_object_count() == 0);
		CHECK(atomic_load(&tracked_frees) == 1);
		CHECK(!logger_init(&next) && !stop_status());
		CHECK(logger_process_object_count() == 0);
		CHECK(atomic_load(&tracked_frees) == 2);
		CHECK(atomic_load(&tracked_destroy_calls) == 2);
		return;
	}

	if (!strcmp(scenario, "candidate-writer-failure")) {
		/* 候选从未对外发布；回滚销毁成功后才能创建新一代。 */
		fail_writer = EDEADLK;
		CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
		CHECK(atomic_load(&create_count) == 1);
		CHECK(atomic_load(&tracked_destroy_calls) == 1);
		CHECK(atomic_load(&tracked_frees) == 1);
		CHECK(logger_process_object_count() == 0);
		CHECK(logger_flush_status() == -1 && errno == ENODEV);
		CHECK(!logger_init(&next) && !stop_status());
		CHECK(atomic_load(&tracked_frees) == 2);
		return;
	}

	if (!strcmp(scenario, "candidate-retain")) {
		/* 写锁获取失败，且候选销毁没有释放回执：禁止后继发布。 */
		fail_writer = EDEADLK;
		atomic_store(&reject_destroy_without_receipt, 1);
		CHECK(logger_init(&cfg) == -1 && errno == EDEADLK);
		CHECK(atomic_load(&tracked_destroy_calls) == 1);
		r7_poisoned_state();
		return;
	}

	CHECK(!logger_init(&cfg));
	CHECK(logger_process_object_count() == 1);
	CHECK(!atomic_load(&tracked_frees));

	if (!strcmp(scenario, "shutdown-writer-failure")) {
		/* 关闭写锁获取失败后回滚 STOPPING，并保留原实例。 */
		fail_writer = EDEADLK;
		CHECK(stop_status() == -1 && errno == EDEADLK);
		CHECK(!atomic_load(&tracked_frees));
		CHECK(logger_process_object_count() == 1);
		CHECK(logger_init(&next) == -1 && errno == EALREADY);
		LOG_INFO("r7-existing-generation");
		CHECK(!logger_flush_status() && !stop_status());
		CHECK(atomic_load(&tracked_frees) == 1);
		CHECK(logger_process_object_count() == 0);
		CHECK(!logger_init(&next) && !stop_status());
		CHECK(atomic_load(&tracked_frees) == 2);
		return;
	}
	if (!strcmp(scenario, "shutdown-unlock-failure")) {
		/* 模拟写锁解锁失败，真实写锁仍持有；禁止再次 free/unlock。 */
		fail_rw_unlock = EIO;
		CHECK(stop_status() == -1 && errno == EIO);
		CHECK(!atomic_load(&tracked_destroy_calls));
		r7_poisoned_state();
		int old_cancel;
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					      &old_cancel));
		CHECK(old_cancel == PTHREAD_CANCEL_DISABLE);
		return;
	}
	if (!strcmp(scenario, "shutdown-retain")) {
		/* 撤销发布成功，但销毁未实际执行，不得清空 g_retired。 */
		atomic_store(&reject_destroy_without_receipt, 1);
		CHECK(stop_status() == -1 && errno == EIO);
		CHECK(atomic_load(&tracked_destroy_calls) == 1);
		r7_poisoned_state();
		return;
	}
	if (!strcmp(scenario, "shutdown-io-receipt")) {
		/* 真实 final free 成功，返回合成 EIO：回执允许后续新代次。 */
		atomic_store(&fail_dispose, 1);
		CHECK(stop_status() == -1 && errno == EIO);
		CHECK(atomic_load(&tracked_destroy_calls) == 1);
		CHECK(atomic_load(&tracked_frees) == 1);
		CHECK(logger_process_object_count() == 0);
		CHECK(!stop_status());
		CHECK(!logger_init(&next) && !stop_status());
		CHECK(atomic_load(&tracked_frees) == 2);
		return;
	}
	CHECK(!"unknown R7 object-stage scenario");
}

static void release_proof_failure(const char *kind)
{
	/* 全局锁失败不需要工作线程；只有线程回收或队列销毁测试
	 * 创建可能保持未回收状态的异步执行器。
	 */
	int asynchronous = !strcmp(kind, "join") ||
			   !strcmp(kind, "cond-destroy");
	logger_config_t cfg = config_for("old.log", asynchronous);
	int error = EIO;
	int lock_uncertain = 0;

	if (!strcmp(kind, "control")) {
		/* 只捕获全局控制互斥锁，不捕获实例互斥锁。 */
		capture_control = 1;
		fail_control_unlock = error;
		CHECK(logger_init(&cfg) == -1 && errno == error);
		CHECK(control_mutex != NULL);
		lock_uncertain = 1;
	} else {
		CHECK(logger_init(&cfg) == 0);
		if (!strcmp(kind, "reader")) {
			fail_rw_unlock = error;
			CHECK(logger_flush_status() == -1 && errno == error);
			lock_uncertain = 1;
		} else if (!strcmp(kind, "writer")) {
			fail_rw_unlock = error;
			CHECK(stop_status() == -1 && errno == error);
			lock_uncertain = 1;
		} else if (!strcmp(kind, "join")) {
			fail_join = error;
			CHECK(stop_status() == -1 && errno == error);
		} else if (!strcmp(kind, "cond-destroy")) {
			fail_cond_destroy = error;
			CHECK(stop_status() == -1 && errno == error);
		} else
			CHECK(!"unknown proof-failure scenario");
	}

	/* 五条路径中旧实例都从未完整释放。
	 * 另一代次或另一次锁获取都不合法。
	 */
	CHECK(logger_process_object_count() == 1);
	pthread_t observer;
	CHECK(!pthread_create(&observer, NULL, verify_poisoned_controller,
				  &error));
	CHECK(!pthread_join(observer, NULL));
	CHECK(access("new.log", F_OK) == -1 && errno == ENOENT);

	int old_state;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state));
	CHECK(old_state == (lock_uncertain ?
				PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE));
	/* 生产库因线程回收失败而故意保留对象。
	 * 仅用于测试：断言 BROKEN 后回收精确的被拒绝工作线程；
	 * 绝不再次调用 logger_destroy，也不重置已失效控制方。
	 * 这为 TSan 在进程退出时提供真实的线程先行发生关系。
	 */
	if (!strcmp(kind, "join")) {
		CHECK(saw_rejected_join);
		CHECK(!__real_pthread_join(rejected_worker, NULL));
	}
	/* 解锁失败仍可能拥有真实锁；绝不尝试清理
	 * 或重新初始化此测试进程。进程退出会回收它。
	 */
}

static void shutdown_error(int real_sync)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	LOG_INFO("dispose-even-on-failure");
	if (real_sync)
		atomic_store(&fail_sync, 1);
	else
		atomic_store(&fail_dispose, 1);
	CHECK(stop_status() == -1 && errno == EIO);
	CHECK(!logger_process_object_count());
	CHECK(!stop_status()); /* 旧失败不是要求重试已释放对象 */
	CHECK(!logger_init(&cfg));
	CHECK(!stop_status());
}
static void contract(void)
{
	CHECK(logger_flush_status() == -1 && errno == ENODEV);
	CHECK(logger_reopen() == -1 && errno == ENODEV);
	CHECK(logger_init(NULL) == -1 && errno == EINVAL);
	CHECK(logger_flush_status() == -1 && errno == ENODEV);
	LOG_INFO("bootstrap-after-failed-create");
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	unsigned n = atomic_load(&create_count);
	CHECK(logger_init(NULL) == -1 && errno == EALREADY);
	CHECK(atomic_load(&create_count) == (int)n);
	errno = ERANGE;
	LOG_INFO("keeps-errno");
	CHECK(errno == ERANGE);
	logger_set_level(LOGGER_DEBUG);
	CHECK(errno == ERANGE);
	CHECK(!logger_flush_status() && errno == ERANGE);
	logger_metrics_t metrics;
	logger_get_global_metrics(&metrics);
	CHECK(errno == ERANGE);
	logger_diagnostics_t diagnostics;
	logger_get_global_diagnostics(&diagnostics);
	CHECK(errno == ERANGE);
	CHECK(diagnostics.state == LOGGER_STATE_RUNNING);
	CHECK(diagnostics.outputs == LOGGER_OUT_FILE);
	logger_file_metrics_t file_metrics;
	CHECK(logger_get_global_file_metrics(&file_metrics) == 0);
	CHECK(errno == ERANGE);
	CHECK(!file_metrics.failed_write_operations);
	logger_set_level((logger_level_t)-1);
	CHECK(errno == EINVAL);
	CHECK(!stop_status());
	all_rejected(ESHUTDOWN);
	CHECK(!stop_status());
	atomic_store(&fail_create, 1);
	CHECK(logger_init(&cfg) == -1 && errno == ENOSPC);
	CHECK(logger_flush_status() == -1 && errno == ESHUTDOWN);
	CHECK(!logger_init(&cfg));
	CHECK(!stop_status());
	CHECK(!logger_process_object_count());
}
static void child_status(void)
{
	logger_config_t cfg = config_for("old.log", 0);
	CHECK(!logger_init(&cfg));
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (stop_status() != -1 || errno != ECHILD)
			_exit(10);
		if (logger_init(&cfg) != -1 || errno != ECHILD)
			_exit(11);
		for (int i = 0; i < OP_COUNT; ++i) {
			call_result_t r = { .op = (enum operation)i };
			invoke(&r);
			if (r.error != ECHILD)
				_exit(20 + i);
		}
		_exit(0);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	LOG_INFO("parent-unaffected");
	CHECK(!stop_status());
}

int main(int argc, char **argv)
{
	CHECK(argc >= 2);
	fixture_reset();
	if (!strcmp(argv[1], "generation")) {
		CHECK(argc == 3);
		generation(parse_operation(argv[2]), 0);
	} else if (!strcmp(argv[1], "bootstrap-generation"))
		generation(OP_WRITE, 1);
	else if (!strcmp(argv[1], "shutdown-generation"))
		shutdown_generation();
	else if (!strcmp(argv[1], "start-gate"))
		start_gate();
	else if (!strcmp(argv[1], "stop-gate"))
		stop_gate();
	else if (!strcmp(argv[1], "closed-readers")) {
		admission_only = 1;
		stop_gate();
	} else if (!strcmp(argv[1], "stop-during-start"))
		during_start_stop();
	else if (!strcmp(argv[1], "double-stop"))
		double_shutdown();
	else if (!strcmp(argv[1], "reentry")) {
		CHECK(argc == 3);
		recursion(argv[2]);
	} else if (!strcmp(argv[1], "acquire-error")) {
		CHECK(argc == 3);
		acquisition_error(argv[2]);
	} else if (!strcmp(argv[1], "release-proof")) {
		CHECK(argc == 3);
		release_proof_failure(argv[2]);
	} else if (!strcmp(argv[1], "r7-stage")) {
		CHECK(argc == 3);
		r7_stage_contract(argv[2]);
	} else if (!strcmp(argv[1], "proof-handoff")) {
		CHECK(argc == 3);
		unlock_shutdown_handoff(!strcmp(argv[2], "acquire-fail"));
#if LOGGER_ENABLE_LOCKDEP
	} else if (!strcmp(argv[1], "publish-cas-poison")) {
		CHECK(argc == 2);
		r7_exact_publication_cas_failure();
#endif
	} else if (!strcmp(argv[1], "shutdown-error"))
		shutdown_error(0);
	else if (!strcmp(argv[1], "shutdown-fsync"))
		shutdown_error(1);
	else if (!strcmp(argv[1], "bootstrap-stop"))
		bootstrap_stop();
	else if (!strcmp(argv[1], "contract"))
		contract();
	else if (!strcmp(argv[1], "child"))
		child_status();
	else
		CHECK(!"unknown scenario");
	puts("global lifecycle regression passed");
	return 0;
}
