#define _GNU_SOURCE
#include "logger_libc.h"
#include "logger_runtime.h"
#include "logger.h"
#include "logger_internal.h"
#include "logger_queue.h"
#include "logger_file.h"
#include "logger_cleanup.h"
#include "logger_uapi.h"
#include "logger_fork_fd.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#define PATH_MAX_ 4096
typedef logger_message_t msg_t;
static int reject_access(void)
{
	if (logger_process_is_child()) {
		errno = ECHILD;
		return 1;
	}
	if (logger_scope_busy()) {
		errno = EDEADLK;
		return 1;
	}
	int rc = logger_process_require();
	if (rc) {
		errno = -rc;
		return 1;
	}
	return 0;
}

logger_detail_t logger_internal_detail(const logger_t *l)
{
	return l->detail;
}
int logger_internal_include_pid(const logger_t *l)
{
	return l->include_pid;
}
int logger_internal_include_tid(const logger_t *l)
{
	return l->include_tid;
}
int logger_internal_include_source(const logger_t *l)
{
	return l->include_source;
}
static _Thread_local logger_context_t g_ctx;
static _Thread_local char g_request_id[64], g_session_id[64], g_trace_id[64];
static void copy_ctx(char *d, size_t n, const char *s)
{
	if (!s) {
		d[0] = 0;
		return;
	}
	size_t length = strnlen(s, n - 1u);
	memcpy(d, s, length);
	d[length] = 0;
}
int logger_context_set(const logger_context_t *c)
{
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);
	if (rc)
		return rc;
	/* 暂存全部字段，允许 set(get()) 以及重叠或交换的 TLS 输入。 */
	char request[64] = { 0 }, session[64] = { 0 }, trace[64] = { 0 };
	copy_ctx(request, sizeof(request), c ? c->request_id : NULL);
	copy_ctx(session, sizeof(session), c ? c->session_id : NULL);
	copy_ctx(trace, sizeof(trace), c ? c->trace_id : NULL);
	memcpy(g_request_id, request, sizeof(request));
	memcpy(g_session_id, session, sizeof(session));
	memcpy(g_trace_id, trace, sizeof(trace));
	g_ctx.request_id = g_request_id[0] ? g_request_id : NULL;
	g_ctx.session_id = g_session_id[0] ? g_session_id : NULL;
	g_ctx.trace_id = g_trace_id[0] ? g_trace_id : NULL;
	/* 恢复取消失败并不撤销已经提交的线程局部上下文。 */
	return logger_scope_end(&scope, 0);
}
int logger_context_clear(void)
{
	return logger_context_set(NULL);
}
int logger_context_get(logger_context_t *out)
{
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);
	if (rc)
		return rc;
	if (!out)
		return logger_scope_end(&scope, -EINVAL);
	/* 指针借用当前线程的存储；取消恢复前不能交付快照。 */
	logger_context_t snapshot = g_ctx;
	rc = logger_scope_end(&scope, 0);
	if (!rc)
		*out = snapshot;
	return rc;
}
static void capture_context(msg_t *m)
{
	logger_context_t c = g_ctx;
	copy_ctx(m->request_id, sizeof(m->request_id), c.request_id);
	copy_ctx(m->session_id, sizeof(m->session_id), c.session_id);
	copy_ctx(m->trace_id, sizeof(m->trace_id), c.trace_id);
}

static pid_t tid_(void)
{
	return (pid_t)syscall(SYS_gettid);
}

static unsigned capture_mask_from_config(const logger_config_t *cfg)
{
	unsigned mask = 0;
	if (cfg->detail >= LOGGER_DETAIL_NORMAL)
		mask |= LOGGER_CAPTURE_MODULE;
	if (cfg->detail >= LOGGER_DETAIL_VERBOSE) {
		if (cfg->include_pid)
			mask |= LOGGER_CAPTURE_PID;
		if (cfg->include_tid)
			mask |= LOGGER_CAPTURE_TID;
	}
	if (cfg->detail >= LOGGER_DETAIL_DEBUG) {
		mask |= LOGGER_CAPTURE_CONTEXT;
		if (cfg->include_source)
			mask |= LOGGER_CAPTURE_SOURCE;
	}
	return mask;
}

static void prepare_record_metadata(logger_t *l, msg_t *m, const char *module,
				    const char *file, int line,
				    const char *function)
{
	unsigned mask = l->capture_mask;
	m->metadata_mask = (uint8_t)(mask & LOGGER_CAPTURE_RECORD_MASK);
	if (mask & LOGGER_CAPTURE_MODULE)
		m->module = module;
	if (mask & LOGGER_CAPTURE_PID)
		m->pid = l->pid;
	if (mask & LOGGER_CAPTURE_TID)
		m->tid = tid_();
	if (mask & LOGGER_CAPTURE_SOURCE) {
		m->file = file;
		m->line = line;
		m->func = function;
	}
	if (mask & LOGGER_CAPTURE_CONTEXT)
		capture_context(m);
}

int logger_get_state(const logger_t *l, logger_state_t *out)
{
	int saved = errno;
	int rc = -EINVAL;

	if (reject_access()) {
		rc = -errno;
		goto done;
	}
	if (!l || !out)
		goto done;
	*out = atomic_load_explicit(&l->state, memory_order_acquire);
	rc = 0;
done:
	errno = saved;
	return rc;
}
static void update_high_watermark(logger_t *l)
{
	uint64_t depth = (uint64_t)logger_queue_depth(&l->q);
	uint64_t old = atomic_load_explicit(&l->queue_high_watermark,
					    memory_order_relaxed);
	while (depth > old &&
	       !atomic_compare_exchange_weak_explicit(
		       &l->queue_high_watermark, &old, depth,
		       memory_order_relaxed, memory_order_relaxed)) {
	}
}

static int vlog_body(logger_t *l, logger_level_t lv, const char *mod,
		     const char *f, int line, const char *fn, const char *fmt,
		     va_list ap, int format_errno)
{
	if (lv < atomic_load_explicit(&l->level, memory_order_relaxed))
		return 0;
	msg_t m = { .level = lv };
	prepare_record_metadata(l, &m, mod, f, line, fn);
	if (clock_gettime(CLOCK_REALTIME, &m.ts) != 0)
		return -(errno ? errno : EIO);
	/* 元数据与时钟调用不能改变正文中 GNU %m 的公开入口值。 */
	errno = format_errno;
	int formatted = logger_libc_vsnprintf(m.text, sizeof(m.text), fmt, ap);
	if (formatted < 0)
		return -(errno ? errno : EILSEQ);
	size_t stored = (size_t)formatted < sizeof(m.text) ?
				(size_t)formatted :
				sizeof(m.text) - 1u;
	if ((size_t)formatted >= sizeof(m.text))
		logger_text_mark_truncated(m.text, sizeof(m.text));
	m.text_len = (uint16_t)stored;

	if (!l->async_mode)
		return logger_emit_status(l, &m, 0);
	int push_rc = logger_queue_push(&l->q, &m);
	if (push_rc > 0) {
		atomic_fetch_add_explicit(&l->enqueued, 1,
					  memory_order_relaxed);
		update_high_watermark(l);
		return 0;
	}
	if (push_rc < 0) {
		/* 记录已发布；通知失败后不能回滚 MPSC 槽位，只能停止接纳。 */
		int error = -push_rc;
		atomic_fetch_add_explicit(&l->enqueued, 1,
					  memory_order_relaxed);
		update_high_watermark(l);
		logger_note_io_error(l, error);
		logger_note_lifecycle_error(l, error);
		int wait_error = logger_queue_wait_error(&l->q);
		if (wait_error) {
			logger_note_synchronization_error(l, wait_error);
		} else {
			int wake_rc = logger_queue_wake_force(&l->q);
			if (wake_rc) {
				logger_note_lifecycle_error(l, -wake_rc);
				wait_error = logger_queue_wait_error(&l->q);
				if (wait_error)
					logger_note_synchronization_error(
						l, wait_error);
			}
		}
		return push_rc;
	}
	if (l->overflow[lv] == LOGGER_OVERFLOW_SYNC) {
		/* 同步回退不计入队列丢弃，返回本次实际 I/O 结果。 */
		atomic_fetch_add_explicit(&l->sync_fallbacks, 1,
					  memory_order_relaxed);
		return logger_emit_status(l, &m, 0);
	}
	atomic_fetch_add_explicit(&l->dropped_by_level[lv], 1,
				  memory_order_relaxed);
	return -EAGAIN;
}
int logger_vlog_internal(logger_t *l, logger_level_t level, const char *module,
			 const char *file, int line, const char *function,
			 const char *fmt, va_list args)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	if (!l || !fmt || (unsigned)level >= LOGGER_OFF)
		rc = -EINVAL;
	else if (atomic_load_explicit(&l->state, memory_order_acquire) !=
		 LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	else
		rc = vlog_body(l, level, module, file, line, function, fmt,
			       args, saved);
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}

static int copy_logger_config(const logger_config_t *in,
			      logger_config_t *out)
{
	if (!in || !out) {
		errno = EINVAL;
		return -1;
	}
	_Static_assert(LOGGER_CONFIG_SIZE == sizeof(*out),
		       "Logger v1 config layout changed");

	uint32_t size, version;
	memcpy(&size, (const unsigned char *)in, sizeof(size));
	memcpy(&version, (const unsigned char *)in + sizeof(size),
	       sizeof(version));
	int rc = logger_uapi_copy_struct(out, sizeof(*out), in, size, version,
					LOGGER_CONFIG_VERSION);
	if (rc) {
		errno = -rc;
		return -1;
	}
	out->struct_size = sizeof(*out);
	out->version = LOGGER_CONFIG_VERSION;
	return 0;
}
static int validate_logger_config(const logger_config_t *cfg)
{
	const unsigned output_mask =
		LOGGER_OUT_STDERR | LOGGER_OUT_FILE | LOGGER_OUT_SYSLOG;
	if ((unsigned)cfg->level > LOGGER_OFF ||
	    (unsigned)cfg->detail > LOGGER_DETAIL_DEBUG || !cfg->outputs ||
	    (cfg->outputs & ~output_mask) ||
	    (unsigned)cfg->rotation.mode > LOGGER_ROTATE_EXTERNAL ||
	    (cfg->file_mode & ~0777u) ||
	    (unsigned)cfg->flush_level > LOGGER_OFF)
		return -EINVAL;
	for (unsigned i = 0; i < 6; ++i)
		if ((unsigned)cfg->overflow[i] > LOGGER_OVERFLOW_SYNC)
			return -EINVAL;
	return 0;
}

typedef struct {
	int emit_mu_ready;
	int progress_mu_ready;
	int progress_cv_ready;
	int queue_ready;
	int worker_started;
} logger_ctor_state_t;

static void logger_ctor_note_proof_error(logger_t *l, int rc)
{
	int error = rc < 0 ? -rc : rc;
	if (!l || !error)
		return;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
		&l->lifecycle_error, &expected, error,
		memory_order_release, memory_order_relaxed);
}

/* 构造候选尚未对外发布，无合法外部借用者；只回滚已完成的资源。
 * 若已启动工作线程，须先停止并 join，才能释放它可访问的存储。
 * 同步对象的 destroy 返回码不是“没有使用者”的证明：调用前就必须
 * 满足静默期前提。销毁/线程回收/计数错误作为额外不变量故障保守保留容器。
 * 调用者保留原始构造 errno；此返回值只用于清理诊断。
 */
static int logger_ctor_rollback(logger_t *l, const logger_ctor_state_t *state)
{
	if (!l)
		return logger_process_object_release();

	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPING,
			      memory_order_release);
	int sync_error = atomic_load_explicit(&l->synchronization_error,
					      memory_order_acquire);
	if (!sync_error && state->queue_ready)
		sync_error = logger_queue_wait_error(&l->q);
	if (sync_error) {
		logger_ctor_note_proof_error(l, sync_error);
		return -sync_error;
	}

	if (state->worker_started) {
		atomic_store_explicit(&l->running, 0, memory_order_release);
		if (state->queue_ready) {
			int wake_rc = logger_queue_wake_force(&l->q);
			if (wake_rc) {
				logger_ctor_note_proof_error(l, wake_rc);
				return wake_rc;
			}
		}
		int join_rc = pthread_join(l->worker, NULL);
		if (join_rc) {
			logger_ctor_note_proof_error(l, join_rc);
			return -join_rc;
		}
	}

	logger_worker_workspace_destroy(l->worker_workspace);
	l->worker_workspace = NULL;

	if (state->queue_ready) {
		int rc = logger_queue_destroy(&l->q);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return rc;
		}
	}

	if (state->progress_cv_ready) {
		int rc = pthread_cond_destroy(&l->progress_cv);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}
	if (state->progress_mu_ready) {
		int rc = pthread_mutex_destroy(&l->progress_mu);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}
	if (state->emit_mu_ready) {
		int rc = pthread_mutex_destroy(&l->emit_mu);
		if (rc) {
			logger_ctor_note_proof_error(l, rc);
			return -rc;
		}
	}

	/* 关闭错误次于构造错误，不会使已证明的内存生命周期变得不安全。
	 * 绝不重试 Linux close。
	 */
	int cleanup_rc = logger_file_close_status(&l->file_backend);
	logger_syslog_close(&l->syslog_backend);

	int release_rc = logger_process_object_release();
	if (release_rc) {
		logger_ctor_note_proof_error(l, release_rc);
		return release_rc;
	}

	int free_rc = logger_runtime_free(l);
	return cleanup_rc ? cleanup_rc : free_rc;
}

static logger_t *create_logger(const logger_config_t *input)
{
	logger_config_t cfg;
	logger_ctor_state_t state = { 0 };
	logger_state_t expected = LOGGER_STATE_CREATED;
	int rc;
	if (copy_logger_config(input, &cfg) != 0)
		return NULL;
	rc = validate_logger_config(&cfg);
	if (rc) {
		errno = -rc;
		return NULL;
	}
	rc = logger_process_object_acquire();
	if (rc) {
		errno = -rc;
		return NULL;
	}
	logger_t *l = logger_runtime_calloc(1, sizeof(*l));
	if (!l) {
		int primary = errno ? errno : ENOMEM;
		/* 没有可保留的容器。存活对象计数回滚失败仍会使进程守卫保持保守；
		 * 绝不替换主要错误 ENOMEM。
		 */
		(void)logger_ctor_rollback(NULL, &state);
		errno = primary;
		return NULL;
	}
	l->file_backend = LOGGER_FILE_EMPTY;
	l->syslog_backend.fd = -1;
	atomic_init(&l->level, cfg.level);
	atomic_init(&l->state, LOGGER_STATE_CREATED);
	atomic_init(&l->default_borrowed, 0);
	atomic_init(&l->running, 0);
	atomic_init(&l->enqueued, 0);
	atomic_init(&l->sync_fallbacks, 0);
	atomic_init(&l->queue_high_watermark, 0);
	atomic_init(&l->consumer_batches, 0);
	atomic_init(&l->consumer_records, 0);
	atomic_init(&l->async_completed, 0);
	atomic_init(&l->sync_completed, 0);
	atomic_init(&l->emitted_records, 0);
	atomic_init(&l->failed_records, 0);
	atomic_init(&l->first_error, 0);
	atomic_init(&l->lifecycle_error, 0);
	atomic_init(&l->synchronization_error, 0);
	for (unsigned i = 0; i < 6; ++i) {
		atomic_init(&l->dropped_by_level[i], 0);
		l->overflow[i] = cfg.overflow[i];
	}
	l->detail = cfg.detail;
	l->outputs = cfg.outputs;
	l->file_mode = (mode_t)(cfg.file_mode ? cfg.file_mode : 0640);
	l->async_mode = !!cfg.async_mode;
	l->flush_level = cfg.flush_level;
	l->include_pid = cfg.include_pid;
	l->include_tid = cfg.include_tid;
	l->include_source = cfg.include_source;
	l->capture_mask = capture_mask_from_config(&cfg);
	l->pid = (l->capture_mask & LOGGER_CAPTURE_PID) ? getpid() : 0;
	copy_ctx(l->ident, sizeof(l->ident), cfg.ident ? cfg.ident : "app");

	rc = pthread_mutex_init(&l->emit_mu, NULL);
	if (rc != 0)
		goto fail;
	state.emit_mu_ready = 1;

	rc = pthread_mutex_init(&l->progress_mu, NULL);
	if (rc != 0)
		goto fail;
	state.progress_mu_ready = 1;

	rc = pthread_cond_init(&l->progress_cv, NULL);
	if (rc != 0)
		goto fail;
	state.progress_cv_ready = 1;

	if ((l->outputs & LOGGER_OUT_FILE) &&
	    logger_file_init(&l->file_backend, cfg.file_path, cfg.rotation,
			     l->file_mode)) {
		rc = errno ? errno : EIO;
		goto fail;
	}

	if ((l->outputs & LOGGER_OUT_SYSLOG) &&
	    logger_syslog_init(&l->syslog_backend, cfg.ident, &cfg.syslog) !=
		    0) {
		rc = errno ? errno : EIO;
		goto fail;
	}

	/* 延迟启动是显式行为，并不声称输出端健康。 */
	if (l->outputs & LOGGER_OUT_SYSLOG)
		logger_note_io_error(l, l->syslog_backend.metrics.last_error);

	if (l->async_mode) {
		if (logger_queue_init(&l->q, cfg.queue_capacity ?
						     cfg.queue_capacity :
						     8192) != 0) {
			rc = errno ? errno : EIO;
			/* 队列的部分构造可能留下未经证明的互斥锁。
			 * 保留其容器和存活对象计数，不替换主要 errno。
			 */
			int proof_error = logger_queue_wait_error(&l->q);
			if (proof_error)
				logger_note_synchronization_error(l, proof_error);
			goto fail;
		}
		state.queue_ready = 1;
		l->worker_workspace = logger_worker_workspace_create(l->q.cap);
		if (!l->worker_workspace) {
			rc = errno ? errno : ENOMEM;
			goto fail;
		}
		atomic_store_explicit(&l->running, 1, memory_order_release);
		rc = pthread_create(&l->worker, NULL, logger_worker_main, l);
		if (rc != 0) {
			atomic_store_explicit(&l->running, 0,
					      memory_order_release);
			goto fail;
		}
		state.worker_started = 1;
	}

	/* 工作线程可能在 pthread_create() 返回前失败。只有 CREATED 可变为 RUNNING：
	 * 绝不覆盖它以 release 发布的 STOPPING 状态。
	 * 失败时的 acquire 会观察先前的 lifecycle_error 发布；
	 * 回滚仍会停止并回收每个成功创建的工作线程。
	 */
	if (!atomic_compare_exchange_strong_explicit(
		    &l->state, &expected, LOGGER_STATE_RUNNING,
		    memory_order_acq_rel, memory_order_acquire)) {
		rc = atomic_load_explicit(&l->lifecycle_error,
					  memory_order_acquire);
		if (!rc)
			rc = EIO;
		goto fail;
	}
	return l;

fail: {
	int primary = rc ? rc : EIO;
	/* 清理诊断绝不替换主要构造失败。
	 * logger_ctor_rollback() 自身强制要求释放前先完成证明。
	 */
	int cleanup_rc = logger_ctor_rollback(l, &state);
	(void)cleanup_rc;
	errno = primary;
	return NULL;
}
}

static void cancel_unreturned_logger(void *arg)
{
	logger_t *l = arg;
	/* scope_end 已在取消生效前清除 TLS 标记。
	 * 销毁从未转交给调用者的对象。
	 */
	(void)logger_dispose_internal(l);
}

/* 将所有权交接与构造分开：清理宏可能实现 setjmp 作用域，
 * 而构造涉及许多可变资源。
 */
static logger_t *finish_create(logger_scope_t *scope, logger_t *l, int error)
{
	pthread_cleanup_push(cancel_unreturned_logger, l);
	(void)logger_scope_end(scope, error);
	if (scope->old_cancel == PTHREAD_CANCEL_ENABLE)
		pthread_testcancel();
	pthread_cleanup_pop(0);
	return l;
}

static logger_t *create_entry(const logger_config_t *input)
{
	logger_scope_t scope;
	if (logger_scope_begin(&scope))
		return NULL;
	/* 启用异步取消时，返回拥有所有权的指针无法成为原子交接。
	 * 在获取资源之前拒绝。全局调用者进入时已禁用取消。
	 */
	int old_type;
	int rc = pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type);
	if (!rc)
		rc = pthread_setcanceltype(old_type, NULL);
	if (rc || (old_type == PTHREAD_CANCEL_ASYNCHRONOUS &&
		   scope.old_cancel == PTHREAD_CANCEL_ENABLE)) {
		(void)logger_scope_end(&scope, rc ? -rc : -ENOTSUP);
		return NULL;
	}
	logger_t *l = create_logger(input);
	int error = l ? 0 : -(errno ? errno : EIO);
	/* 在所有权交接前处理待定的延迟取消。清理在禁用取消时注册；
	 * pop/return 后不再有 API 取消点。
	 * 调用者仍必须为之后的取消安装自己的清理处理。
	 */
	return finish_create(&scope, l, error);
}

int logger_create(const logger_config_t *input, logger_t **out)
{
	int saved = errno;
	int rc = 0;

	/* out 是调用者独占的拥有者槽。禁止覆盖已有所有者，避免静默泄漏。
	 * 子方准入先于任何运行态访问；失败不发布候选对象。
	 */
	if (reject_access()) {
		rc = -errno;
	} else if (!out) {
		rc = -EINVAL;
	} else if (*out) {
		rc = -EBUSY;
	} else {
		logger_t *candidate = create_entry(input);
		if (candidate)
			*out = candidate;
		else
			rc = -(errno ? errno : EIO);
	}
	errno = saved;
	return rc;
}

static int dispose_body(logger_t *l, int *released)
{
	if (!l) {
		if (released)
			*released = 1;
		return 0;
	}

	/* 前提：宿主已停止并等待全部外部借用者结束，禁止新的原始指针访问。
	 * 状态 STOPPING 不能自动排空外部借用者；成功 join 只证明内部工作线程
	 * 已退出。满足这些先决条件后再销毁同步对象和依赖存储；
	 * pthread_*_destroy 成功返回不能代替静默期证明。
	 * 不变量检查失败时保留半退役对象，避免释放后使用。
	 */
	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPING,
			      memory_order_release);
	int sync_error = atomic_load_explicit(&l->synchronization_error,
					      memory_order_acquire);
	if (!sync_error && l->async_mode)
		sync_error = logger_queue_wait_error(&l->q);
	if (sync_error) {
		logger_note_lifecycle_error(l, sync_error);
		return -sync_error;
	}

	if (l->async_mode) {
		atomic_store_explicit(&l->running, 0, memory_order_release);
		int wake_rc = logger_queue_wake_force(&l->q);
		if (wake_rc) {
			logger_note_lifecycle_error(l, -wake_rc);
			int wait_error = logger_queue_wait_error(&l->q);
			if (wait_error)
				logger_note_synchronization_error(l, wait_error);
			return wake_rc;
		}
		int join_rc = pthread_join(l->worker, NULL);
		if (join_rc) {
			int expected = 0;
			(void)atomic_compare_exchange_strong_explicit(
				&l->lifecycle_error, &expected, join_rc,
				memory_order_release, memory_order_relaxed);
			return -join_rc;
		}
	}
	sync_error = atomic_load_explicit(&l->synchronization_error,
					 memory_order_acquire);
	if (sync_error)
		return -sync_error;

	int lifecycle_error = atomic_load_explicit(
		&l->lifecycle_error, memory_order_acquire);

	int lock_rc = __cleanup_lockdep_mutex_lock(&l->emit_mu, LOGGER_LOCK_INSTANCE_EMIT);
	if (lock_rc) {
		atomic_store_explicit(&l->lifecycle_error, lock_rc,
				      memory_order_release);
		return -lock_rc;
	}
	int rc = lifecycle_error ? -lifecycle_error :
		 logger_sync_outputs_locked(l);
	int unlock_rc = __cleanup_lockdep_mutex_unlock(&l->emit_mu, LOGGER_LOCK_INSTANCE_EMIT);
	if (unlock_rc) {
		logger_note_synchronization_error(l, unlock_rc);
		return -unlock_rc;
	}

	/* 工作线程已不存在，因此其私有工作区不再被共享。 */
	if (l->async_mode) {
		logger_worker_workspace_destroy(l->worker_workspace);
		l->worker_workspace = NULL;
		int qrc = logger_queue_destroy(&l->q);
		if (qrc) {
			atomic_store_explicit(&l->lifecycle_error, -qrc,
					      memory_order_release);
			return qrc;
		}
	}

	/* 外部借用者已静止且工作线程已 join；以下销毁只做额外不变量
	 * 检查，不是检测并发使用者的通用机制。失败时停止最终释放。
	 */
	int destroy_rc = pthread_cond_destroy(&l->progress_cv);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}
	destroy_rc = pthread_mutex_destroy(&l->progress_mu);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}
	destroy_rc = pthread_mutex_destroy(&l->emit_mu);
	if (destroy_rc) {
		atomic_store_explicit(&l->lifecycle_error, destroy_rc,
				      memory_order_release);
		return -destroy_rc;
	}

	int close_rc = logger_file_close_status(&l->file_backend);
	if (!rc)
		rc = close_rc;
	if (l->syslog_backend.fd >= 0) {
		int fd = l->syslog_backend.fd;
		l->syslog_backend.fd = -1;
		if (logger_fork_close(fd) < 0 && !rc)
			rc = -errno;
	}

	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPED,
			      memory_order_release);

	/* 进程级存活对象计数是不变量门禁。不得通过释放对象并假装没有运行时
	 * 来掩盖计数下溢。
	 */
	int release_rc = logger_process_object_release();
	if (release_rc)
		return release_rc;

	/* 映射释放可能失败；只有真实归还成功才发布最终释放回执。
	 * 失败映射仍在进程域登记中，不因对象计数归零就假称已释放。
	 */
	int free_rc = logger_runtime_free(l);
	if (free_rc)
		return rc ? rc : free_rc;
	if (released)
		*released = 1;
	int domain_error =
		atomic_load_explicit(&logger_runtime_current()->resource_error,
				     memory_order_acquire);
	return rc ? rc : -domain_error;
}

static int logger_dispose_internal_tracked(logger_t *l, int *released)
{
	if (released)
		*released = 0;
	/* 销毁 NULL 保留其不注册、不获取资源的行为。 */
	if (!l) {
		int rc = reject_access() ? -errno : 0;
		if (!rc && released)
			*released = 1;
		return rc;
	}
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);
	if (rc)
		return rc;
	rc = dispose_body(l, released);
	return logger_scope_end(&scope, rc);
}

int logger_dispose_internal(logger_t *l)
{
	return logger_dispose_internal_tracked(l, NULL);
}

static _Thread_local int destroy_receipt;

void logger_destroy_receipt_reset(void)
{
	destroy_receipt = 0;
}

int logger_destroy_receipt_read(void)
{
	return destroy_receipt;
}

int logger_destroy(logger_t **owner)
{
	int saved = errno;
	int rc;
	logger_scope_t scope;

	logger_destroy_receipt_reset();
	/* 拒绝发生在读取拥有者槽和消费其内容之前。普通调用者必须提供
	 * 存活、独占且可写的槽；其他别名不是独立拥有者。
	 */
	if (reject_access()) {
		rc = -errno;
		goto out;
	}
	if (!owner) {
		rc = -EINVAL;
		goto out;
	}
	if (!*owner) {
		destroy_receipt = 1;
		rc = 0;
		goto out;
	}
	rc = logger_scope_begin(&scope);
	if (rc)
		goto out;

	/* 准入成功后消费公开所有权，先清槽再做不可回滚的拆除。
	 * I/O 或生命周期证明失败均不允许调用者重试该指针；证明失败时
	 * 旧分配继续由进程域保守保留。清槽早于取消恢复，取消清理不会
	 * 再次释放同一个对象。最终释放回执供内部 Global 单独判定。
	 */
	logger_t *instance = *owner;
	if (atomic_load_explicit(&instance->default_borrowed,
				 memory_order_acquire)) {
		rc = logger_scope_end(&scope, -EBUSY);
		goto out;
	}
	*owner = NULL;
	rc = dispose_body(instance, &destroy_receipt);
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_log(logger_t *l, logger_level_t level, const char *module,
	       const char *file, int line, const char *function,
	       const char *fmt, ...)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	rc = !l || !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		rc = vlog_body(l, level, module, file, line, function, fmt, ap,
			       saved);
		va_end(ap);
	}
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}
int logger_log_source(logger_t *l, logger_level_t level, logger_source_t source,
		      const char *fmt, ...)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	rc = !l || !fmt || (unsigned)level >= LOGGER_OFF ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		va_list ap;
		va_start(ap, fmt);
		rc = vlog_body(l, level, source.module, source.file,
			       source.line, source.func, fmt, ap, saved);
		va_end(ap);
	}
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_log_sync_status(logger_t *l, logger_level_t level,
			   const char *module, const char *file, int line,
			   const char *function, const char *fmt, ...)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	if (!l || !fmt || (unsigned)level >= LOGGER_OFF)
		rc = -EINVAL;
	else if (atomic_load_explicit(&l->state, memory_order_acquire) !=
		 LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	else if (level >=
		 atomic_load_explicit(&l->level, memory_order_relaxed)) {
		logger_message_t msg = { .level = level };
		prepare_record_metadata(l, &msg, module, file, line, function);
		if (clock_gettime(CLOCK_REALTIME, &msg.ts))
			rc = -(errno ? errno : EIO);
		else {
			va_list ap;
			va_start(ap, fmt);
			errno = saved;
			int n = logger_libc_vsnprintf(
				msg.text, sizeof(msg.text), fmt, ap);
			va_end(ap);
			if (n < 0)
				rc = -(errno ? errno : EILSEQ);
			else if ((size_t)n >= sizeof(msg.text))
				rc = -EOVERFLOW;
			else
				rc = logger_emit_status(l, &msg, 1);
		}
	}
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}

int logger_reopen(logger_t *l)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	rc = !l || !(l->outputs & LOGGER_OUT_FILE) ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
		rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
		if (!rc) {
			rc = logger_file_reopen(&l->file_backend) ?
				     -(errno ? errno : EIO) :
				     0;
			if (rc)
				logger_note_io_error(l, -rc);
		}
		if (!ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard)) {
			int unlock_rc = RELEASE_ERR(pthread_mutex_emit,
						    &emit_guard);
			if (unlock_rc) {
				logger_note_synchronization_error(l, unlock_rc);
				if (!rc)
					rc = unlock_rc;
			}
		}
	}
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}
void logger_note_io_error(logger_t *l, int error)
{
	if (error <= 0)
		return;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(&l->first_error,
						      &expected, error,
						      memory_order_release,
						      memory_order_relaxed);
}

void logger_note_lifecycle_error(logger_t *l, int error)
{
	if (!l || !error)
		return;
	if (error < 0)
		error = -error;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
		&l->lifecycle_error, &expected, error, memory_order_release,
		memory_order_relaxed);
	atomic_store_explicit(&l->state, LOGGER_STATE_STOPPING,
			      memory_order_release);
	atomic_store_explicit(&l->running, 0, memory_order_release);
}

void logger_note_synchronization_error(logger_t *l, int error)
{
	if (!l || !error)
		return;
	if (error < 0)
		error = -error;
	int expected = 0;
	(void)atomic_compare_exchange_strong_explicit(
		&l->synchronization_error, &expected, error,
		memory_order_release, memory_order_relaxed);
	logger_note_lifecycle_error(l, error);
}

int logger_wait_for_output(logger_t *l)
{
	if (logger_process_is_child())
		return -ECHILD;
	if (!l)
		return -EINVAL;
	int sync_error = atomic_load_explicit(&l->synchronization_error,
					      memory_order_acquire);
	if (sync_error)
		return -sync_error;
	if (l->async_mode) {
		/* 顺序由预留计数器定义，而不是 enqueued 统计。
		 * 快照前已预留但尚未发布的槽位也必须计入，
		 * 慢生产者不能被后续记录的完成位置越过。
		 */
		size_t target = atomic_load_explicit(&l->q.enqueue_pos,
						     memory_order_acquire);
		ACQUIRE(pthread_mutex_progress_checked, progress_guard)(&l->progress_mu);
		int rc = ACQUIRE_ERR(pthread_mutex_progress_checked, &progress_guard);
		if (rc)
			return rc;
		int result = 0;
		for (;;) {
			/* 执行器失败后不能保证该水位。每次唤醒后都在通知互斥锁下
			 * 检查不会自动清除的错误状态，不能伪造队列空洞的完成状态。
			 */
			int error = atomic_load_explicit(&l->lifecycle_error,
						 memory_order_acquire);
			if (error) {
				result = -error;
				break;
			}
			if (l->completed_pos - target <= SIZE_MAX / 2)
				break;
			rc = pthread_cond_wait(&l->progress_cv,
					       &l->progress_mu);
			if (rc != 0) {
				result = -rc;
				break;
			}
		}
		int unlock_rc = RELEASE_ERR(pthread_mutex_progress,
					    &progress_guard);
		if (unlock_rc) {
			logger_note_synchronization_error(l, unlock_rc);
			if (!result)
				result = unlock_rc;
		}
		if (result)
			return result;
	}
	int error = atomic_load_explicit(&l->first_error, memory_order_acquire);
	return error ? -error : 0;
}

int logger_sync_outputs_locked(logger_t *l)
{
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &l->emit_mu);
	if (l->outputs & LOGGER_OUT_FILE) {
		int rc = logger_file_sync(&l->file_backend);
		if (rc < 0)
			logger_note_io_error(l, -rc);
	}
	int error = atomic_load_explicit(&l->first_error, memory_order_acquire);
	return error ? -error : 0;
}

int logger_flush(logger_t *l)
{
	int saved = errno;
	logger_scope_t scope;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto out;
	rc = !l ? -EINVAL : 0;
	if (!rc && atomic_load_explicit(&l->state, memory_order_acquire) !=
			   LOGGER_STATE_RUNNING)
		rc = -ESHUTDOWN;
	if (!rc) {
		/* 预留到通知、等待到同步的整段都处于禁用取消的作用域；
		 * 等待工作线程进度时不能持有 emit_mu。
		 */
		rc = logger_wait_for_output(l);
		/* 等待期间，运行中的工作线程可能已失去 emit_mu 释放证明。
		 * 绝不重新获取已知未经证明的锁。普通后端错误仍要求执行下方
		 * 尽力而为的最终同步。
		 */
		int sync_error = atomic_load_explicit(&l->synchronization_error,
						      memory_order_acquire);
		int sync_rc = sync_error ? -sync_error : 0;
		if (!sync_error) {
			ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
			sync_rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
			if (!sync_rc)
				sync_rc = logger_sync_outputs_locked(l);
			if (!ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard)) {
				int unlock_rc = RELEASE_ERR(
					pthread_mutex_emit, &emit_guard);
				if (unlock_rc) {
					logger_note_synchronization_error(l,
								  unlock_rc);
					if (!sync_rc)
						sync_rc = unlock_rc;
				}
			}
		}
		if (!rc)
			rc = sync_rc;
	}
	rc = logger_scope_end(&scope, rc);
out:
	errno = saved;
	return rc;
}

void logger_set_instance_level(logger_t *l, logger_level_t v)
{
	if (reject_access())
		return;
	if ((unsigned)v > LOGGER_OFF) {
		errno = EINVAL;
		return;
	}
	if (l)
		atomic_store_explicit(&l->level, v, memory_order_relaxed);
}
int logger_dropped(const logger_t *l, uint64_t *out)
{
	int saved = errno;
	int rc = -EINVAL;
	uint64_t n = 0;

	if (reject_access()) {
		rc = -errno;
		goto done;
	}
	if (!l || !out)
		goto done;
	for (unsigned i = 0; i < 6; i++)
		n += atomic_load_explicit(&l->dropped_by_level[i],
					  memory_order_relaxed);
	*out = n;
	rc = 0;
done:
	errno = saved;
	return rc;
}
int logger_get_metrics(const logger_t *l, logger_metrics_t *out)
{
	int saved = errno;
	int rc = -EINVAL;

	if (reject_access()) {
		rc = -errno;
		goto done;
	}
	if (!l || !out)
		goto done;
	memset(out, 0, sizeof(*out));
	for (unsigned i = 0; i < 6; i++)
		out->dropped[i] = atomic_load_explicit(&l->dropped_by_level[i],
						       memory_order_relaxed);
	out->queue_high_watermark = atomic_load_explicit(
		&l->queue_high_watermark, memory_order_relaxed);
	out->enqueued =
		atomic_load_explicit(&l->enqueued, memory_order_relaxed);
	out->sync_fallbacks =
		atomic_load_explicit(&l->sync_fallbacks, memory_order_relaxed);
	out->consumer_batches = atomic_load_explicit(&l->consumer_batches,
						     memory_order_relaxed);
	out->consumer_records = atomic_load_explicit(&l->consumer_records,
						     memory_order_relaxed);
	rc = 0;
done:
	errno = saved;
	return rc;
}

int logger_get_io_metrics(const logger_t *l, logger_io_metrics_t *out)
{
	int saved = errno;
	int rc = -EINVAL;

	if (reject_access()) {
		rc = -errno;
		goto done;
	}
	if (!l || !out)
		goto done;
	memset(out, 0, sizeof(*out));
	out->async_completed =
		atomic_load_explicit(&l->async_completed, memory_order_acquire);
	out->sync_completed =
		atomic_load_explicit(&l->sync_completed, memory_order_acquire);
	out->emitted_records =
		atomic_load_explicit(&l->emitted_records, memory_order_relaxed);
	out->failed_records =
		atomic_load_explicit(&l->failed_records, memory_order_relaxed);
	out->first_error =
		atomic_load_explicit(&l->first_error, memory_order_acquire);
	rc = 0;
done:
	errno = saved;
	return rc;
}

int logger_get_diagnostics(const logger_t *l, logger_diagnostics_t *out)
{
	int saved = errno;
	int rc = -EINVAL;

	if (reject_access()) {
		rc = -errno;
		goto done;
	}
	if (!l || !out)
		goto done;
	memset(out, 0, sizeof(*out));

	out->state = atomic_load_explicit(&l->state, memory_order_acquire);
	out->level = atomic_load_explicit(&l->level, memory_order_relaxed);
	out->outputs = l->outputs;
	out->async_mode = l->async_mode;
	out->worker_running =
		l->async_mode ?
			atomic_load_explicit(&l->running, memory_order_acquire) :
			0;

	for (unsigned i = 0; i < 6; ++i)
		out->dropped_records += atomic_load_explicit(
			&l->dropped_by_level[i], memory_order_relaxed);
	out->sync_fallbacks =
		atomic_load_explicit(&l->sync_fallbacks, memory_order_relaxed);
	out->enqueued =
		atomic_load_explicit(&l->enqueued, memory_order_relaxed);
	out->consumer_batches =
		atomic_load_explicit(&l->consumer_batches, memory_order_relaxed);
	out->consumer_records =
		atomic_load_explicit(&l->consumer_records, memory_order_relaxed);
	out->async_completed =
		atomic_load_explicit(&l->async_completed, memory_order_acquire);
	out->sync_completed =
		atomic_load_explicit(&l->sync_completed, memory_order_acquire);
	out->emitted_records =
		atomic_load_explicit(&l->emitted_records, memory_order_relaxed);
	out->failed_records =
		atomic_load_explicit(&l->failed_records, memory_order_relaxed);
	out->first_error =
		atomic_load_explicit(&l->first_error, memory_order_acquire);

	if (l->async_mode) {
		out->queue_capacity = (uint64_t)l->q.cap;
		out->queue_depth = (uint64_t)logger_queue_depth(&l->q);
		out->queue_high_watermark = atomic_load_explicit(
			&l->queue_high_watermark, memory_order_relaxed);
		out->queue_storage_bytes =
			(uint64_t)logger_queue_storage_bytes(&l->q);
		out->spill_capacity =
			(uint64_t)logger_queue_spill_capacity(&l->q);
		out->spill_in_use =
			(uint64_t)logger_queue_spill_in_use(&l->q);
		out->spill_exhaustions =
			logger_queue_spill_exhaustions(&l->q);
		out->consumer_waiting = (int)atomic_load_explicit(
			&l->q.consumer_waiting, memory_order_acquire);
		out->worker_wait_count = logger_queue_wait_count(&l->q);
		out->producer_wake_signals =
			logger_queue_producer_wake_signals(&l->q);
		out->force_wake_signals =
			logger_queue_force_wake_signals(&l->q);
		out->completion_backlog =
			out->enqueued > out->async_completed ?
				out->enqueued - out->async_completed :
				0;
	}

	if (out->dropped_records)
		out->observation_flags |= LOGGER_DIAG_DROPS_OBSERVED;
	if (out->sync_fallbacks)
		out->observation_flags |= LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED;
	if (out->queue_capacity &&
	    out->queue_high_watermark >= out->queue_capacity)
		out->observation_flags |= LOGGER_DIAG_QUEUE_SATURATED;
	if (out->spill_exhaustions)
		out->observation_flags |=
			LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED;
	if (out->failed_records || out->first_error)
		out->observation_flags |= LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED;
	if (out->async_mode && out->state == LOGGER_STATE_RUNNING &&
	    !out->worker_running)
		out->observation_flags |= LOGGER_DIAG_WORKER_STOPPED;
	rc = 0;
done:
	errno = saved;
	return rc;
}

int logger_file_metrics_snapshot(logger_t *l, logger_file_metrics_t *out)
{
	if (!l || !out)
		return -EINVAL;
	if (!(l->outputs & LOGGER_OUT_FILE))
		return -ENOTSUP;
	int sync_error = atomic_load_explicit(&l->synchronization_error,
					      memory_order_acquire);
	if (sync_error)
		return -sync_error;
	ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
	int rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
	if (rc)
		return rc;
	logger_file_metrics_t snapshot = l->file_backend.metrics;
	snapshot.current_size = (uint64_t)l->file_backend.current_size;
	snapshot.detached = l->file_backend.detached;
	rc = RELEASE_ERR(pthread_mutex_emit, &emit_guard);
	if (rc) {
		logger_note_synchronization_error(l, rc);
		return rc;
	}
	*out = snapshot;
	return 0;
}

int logger_get_file_metrics(logger_t *l, logger_file_metrics_t *out)
{
	int saved = errno;
	logger_scope_t scope;
	logger_file_metrics_t snapshot;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto done;
	rc = !out ? -EINVAL : logger_file_metrics_snapshot(l, &snapshot);
	rc = logger_scope_end(&scope, rc);
	/* 取消恢复可能失败或运行调用者清理；此后只发布局部值。 */
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

int logger_get_syslog_metrics(logger_t *l, logger_syslog_metrics_t *out)
{
	int saved = errno;
	logger_scope_t scope;
	logger_syslog_metrics_t snapshot;
	int rc = logger_scope_begin(&scope);

	if (rc)
		goto done;
	rc = !l || !out ? -EINVAL : 0;
	if (!rc && !(l->outputs & LOGGER_OUT_SYSLOG))
		rc = -ENOTSUP;
	if (!rc) {
		int sync_error = atomic_load_explicit(
			&l->synchronization_error, memory_order_acquire);
		if (sync_error)
			rc = -sync_error;
	}
	if (!rc) {
		ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
		rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
		if (!rc) {
			snapshot = l->syslog_backend.metrics;
			int unlock_rc = RELEASE_ERR(pthread_mutex_emit,
						    &emit_guard);
			if (unlock_rc) {
				logger_note_synchronization_error(l, unlock_rc);
				rc = unlock_rc;
			}
		}
	}
	rc = logger_scope_end(&scope, rc);
	if (!rc)
		*out = snapshot;
done:
	errno = saved;
	return rc;
}

void logger_context_reset_child(void)
{
	g_ctx = (logger_context_t){ 0 };
	g_request_id[0] = 0;
	g_session_id[0] = 0;
	g_trace_id[0] = 0;
	logger_destroy_receipt_reset();
}
