#define _GNU_SOURCE
#include "logger_internal.h"
#include "logger_cleanup.h"
#include "logger_sigpipe.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define BATCH_MAX 256

struct logger_worker_workspace {
	size_t capacity;
	logger_message_t *batch;
	struct iovec *vec;
	struct iovec *copy;
	char *lines;
	size_t *lens;
	unsigned char *failed;
};

static char *workspace_line(logger_worker_workspace_t *workspace, size_t index)
{
	return workspace->lines + index * LOGGER_LINE_MAX;
}

logger_worker_workspace_t *logger_worker_workspace_create(size_t queue_capacity)
{
	size_t capacity = queue_capacity < BATCH_MAX ? queue_capacity : BATCH_MAX;
	if (!capacity)
		capacity = 1;

	logger_worker_workspace_t *workspace __free(free) =
		calloc(1, sizeof(*workspace));
	logger_message_t *batch __free(free) =
		calloc(capacity, sizeof(*batch));
	struct iovec *vec __free(free) = calloc(capacity, sizeof(*vec));
	struct iovec *copy __free(free) = calloc(capacity, sizeof(*copy));
	char *lines __free(free) = malloc(capacity * LOGGER_LINE_MAX);
	size_t *lens __free(free) = calloc(capacity, sizeof(*lens));
	unsigned char *failed __free(free) =
		calloc(capacity, sizeof(*failed));

	if (!workspace || !batch || !vec || !copy || !lines || !lens ||
	    !failed) {
		errno = ENOMEM;
		return NULL;
	}

	workspace->capacity = capacity;
	/* ownership transfer：词法 allocation owner -> workspace object。
	 * 最终由 logger_worker_workspace_destroy() 释放。 */
	workspace->batch = no_free_ptr(batch);
	workspace->vec = no_free_ptr(vec);
	workspace->copy = no_free_ptr(copy);
	workspace->lines = no_free_ptr(lines);
	workspace->lens = no_free_ptr(lens);
	workspace->failed = no_free_ptr(failed);
	/* ownership transfer：workspace constructor -> caller/logger instance。 */
	return_ptr(workspace);
}

void logger_worker_workspace_destroy(logger_worker_workspace_t *workspace)
{
	if (!workspace)
		return;
	free(workspace->failed);
	free(workspace->lens);
	free(workspace->lines);
	free(workspace->copy);
	free(workspace->vec);
	free(workspace->batch);
	free(workspace);
}


int logger_stderr_writev_all(struct iovec *v, int count)
{
	logger_sigpipe_guard_t sigpipe_guard;
	int result = logger_sigpipe_begin(&sigpipe_guard);
	if (result)
		return result;

	int first = 0;
	while (first < count) {
		ssize_t n = writev(STDERR_FILENO, v + first, count - first);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			result = -(errno ? errno : EIO);
			break;
		}
		if (n == 0) {
			result = -EIO;
			break;
		}
		size_t left = (size_t)n;
		while (first < count && left >= v[first].iov_len) {
			left -= v[first].iov_len;
			++first;
		}
		if (first < count && left != 0) {
			v[first].iov_base = (char *)v[first].iov_base + left;
			v[first].iov_len -= left;
		}
	}
	return logger_sigpipe_end(&sigpipe_guard, result, result == -EPIPE);
}

int logger_emit_status(logger_t *l, const logger_message_t *m, int force_sync)
{
	char line[LOGGER_LINE_MAX];
	size_t n;
	int rc = 0;

	/* 同步确认接口必须在任何输出端看到部分内容前
	 * 拒绝格式错误或被截断的记录；普通日志和 worker 批次维持尽力语义。 */
	if (force_sync) {
		rc = logger_format_line_checked(l, m, line, sizeof(line), &n);
		if (rc)
			return rc;
	} else {
		n = logger_format_line(l, m, line, sizeof(line));
	}
	int sync_error = atomic_load_explicit(&l->synchronization_error,
					     memory_order_acquire);
	if (sync_error) {
		logger_note_io_error(l, sync_error);
		atomic_fetch_add_explicit(&l->failed_records, 1,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&l->sync_completed, 1,
					  memory_order_release);
		return -sync_error;
	}
	ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
	rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
	if (rc) {
		logger_note_io_error(l, -rc);
		atomic_fetch_add_explicit(&l->failed_records, 1,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(&l->sync_completed, 1,
					  memory_order_release);
		return rc;
	}
	if (l->outputs & LOGGER_OUT_STDERR) {
		struct iovec v = { .iov_base = line, .iov_len = n };
		rc = logger_stderr_writev_all(&v, 1);
	}
	if (l->outputs & LOGGER_OUT_FILE) {
		int x = logger_file_write(&l->file_backend, line, n, &m->ts,
					  force_sync ||
						  m->level >= l->flush_level);
		if (rc == 0)
			rc = x;
	}
	if (l->outputs & LOGGER_OUT_SYSLOG) {
		if (logger_syslog_write(&l->syslog_backend, m->level, line,
					n) != 0 &&
		    rc == 0)
			rc = -(errno ? errno : EIO);
	}
	int unlock_rc = RELEASE_ERR(pthread_mutex_emit, &emit_guard);
	if (unlock_rc) {
		logger_note_synchronization_error(l, unlock_rc);
		if (!rc)
			rc = unlock_rc;
	}
	if (rc < 0) {
		logger_note_io_error(l, -rc);
		atomic_fetch_add_explicit(&l->failed_records, 1,
					  memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&l->emitted_records, 1,
					  memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&l->sync_completed, 1, memory_order_release);
	return rc;
}

static void emit_batch(logger_t *l, logger_worker_workspace_t *workspace,
		       size_t count)
{
	memset(workspace->failed, 0, count);
	for (size_t i = 0; i < count; ++i) {
		char *line = workspace_line(workspace, i);
		workspace->lens[i] =
			logger_format_line(l, &workspace->batch[i], line,
					   LOGGER_LINE_MAX);
		workspace->vec[i].iov_base = line;
		workspace->vec[i].iov_len = workspace->lens[i];
	}

	ACQUIRE(pthread_mutex_emit_checked, emit_guard)(&l->emit_mu);
	int lock_rc = ACQUIRE_ERR(pthread_mutex_emit_checked, &emit_guard);
	if (lock_rc) {
		memset(workspace->failed, 1, count);
		logger_note_io_error(l, -lock_rc);
		logger_note_lifecycle_error(l, -lock_rc);
		goto account_batch;
	}
	if (l->outputs & LOGGER_OUT_STDERR) {
		memcpy(workspace->copy, workspace->vec,
		       count * sizeof(*workspace->vec));
		int rc = logger_stderr_writev_all(workspace->copy, (int)count);
		if (rc < 0) {
			/* partial batch 没有逐条 acknowledgement。
			 * 因此保守地把整批 record 标记为未确认。 */
			memset(workspace->failed, 1, count);
			logger_note_io_error(l, -rc);
		}
	}
	if (l->outputs & LOGGER_OUT_FILE) {
		size_t total = 0;
		int need_sync = 0;
		for (size_t i = 0; i < count; ++i) {
			total += workspace->lens[i];
			if (workspace->batch[i].level >= l->flush_level)
				need_sync = 1;
		}
		memcpy(workspace->copy, workspace->vec,
		       count * sizeof(*workspace->vec));
		int rc = logger_file_writev(
			&l->file_backend, workspace->copy, (int)count, total,
			&workspace->batch[count - 1].ts, need_sync);
		if (rc < 0) {
			memset(workspace->failed, 1, count);
			logger_note_io_error(l, -rc);
		}
	}
	if (l->outputs & LOGGER_OUT_SYSLOG) {
		for (size_t i = 0; i < count; ++i) {
			if (logger_syslog_write(
				    &l->syslog_backend, workspace->batch[i].level,
				    workspace_line(workspace, i),
				    workspace->lens[i]) != 0) {
				workspace->failed[i] = 1;
				logger_note_io_error(l, errno ? errno : EIO);
			}
		}
	}
	int unlock_rc = RELEASE_ERR(pthread_mutex_emit, &emit_guard);
	if (unlock_rc) {
		memset(workspace->failed, 1, count);
		logger_note_io_error(l, -unlock_rc);
		logger_note_synchronization_error(l, unlock_rc);
	}

account_batch:
	;
	uint64_t bad = 0;
	for (size_t i = 0; i < count; ++i)
		bad += workspace->failed[i];
	atomic_fetch_add_explicit(&l->failed_records, bad,
				  memory_order_relaxed);
	atomic_fetch_add_explicit(&l->emitted_records, count - bad,
				  memory_order_relaxed);
}

void *logger_worker_main(void *p)
{
	logger_t *l = p;
	logger_worker_workspace_t *workspace = l->worker_workspace;
	logger_scope_worker_enter();
	for (;;) {
		size_t n = logger_queue_drain(&l->q, workspace->batch,
					      workspace->capacity);
		if (n != 0) {
			/* 保留 legacy metrics 作为 dequeue 统计。下面的 completion 是另一个事件，
			 * backend 输出失败也会推进 completion。 */
			atomic_fetch_add_explicit(&l->consumer_batches, 1,
						  memory_order_relaxed);
			atomic_fetch_add_explicit(&l->consumer_records, n,
						  memory_order_relaxed);
			emit_batch(l, workspace, n);
			atomic_fetch_add_explicit(&l->async_completed, n,
						  memory_order_release);
			if (atomic_load_explicit(&l->synchronization_error,
						 memory_order_acquire))
				break;

			{
				ACQUIRE(pthread_mutex_progress_checked, progress_guard)(
					&l->progress_mu);
				int lock_rc = ACQUIRE_ERR(
					pthread_mutex_progress_checked,
					&progress_guard);
				if (lock_rc) {
					logger_note_lifecycle_error(l, -lock_rc);
					continue;
				}
				l->completed_pos = atomic_load_explicit(
					&l->q.dequeue_pos, memory_order_acquire);
				pthread_cond_broadcast(&l->progress_cv);
				int unlock_rc = RELEASE_ERR(
					pthread_mutex_progress,
					&progress_guard);
				if (unlock_rc) {
					logger_note_synchronization_error(l,
								  unlock_rc);
					break;
				}
			}
			continue;
		}
		int stop;
		{
			ACQUIRE(pthread_mutex_queue_wait_checked, wait_guard)(
				&l->q.wait_mu);
			int lock_rc = ACQUIRE_ERR(
				pthread_mutex_queue_wait_checked, &wait_guard);
			if (lock_rc) {
				logger_note_lifecycle_error(l, -lock_rc);
				break;
			}
			/*
			 * waiting 必须在持有 wait_mu 时 publish，并且 publish 后重新检查
			 * queue/running。这样 producer 如果在任一窗口发布 record：
			 * - 没看到 waiting：本次 recheck 会看到 record；
			 * - 看到了 waiting：会在同一 mutex 下 signal。
			 */
			while (logger_queue_empty(&l->q) &&
			       atomic_load_explicit(&l->running,
						    memory_order_acquire)) {
				/*
				 * arm 是 acq_rel RMW，随后立即 recheck。若 producer
				 * probe 已先发生，arm acquire 取得其 publication chain；
				 * 否则后续 producer 会观察 waiting=1 并走 signal。
				 */
				logger_queue_wait_arm_recheck(&l->q);
				if (!logger_queue_empty(&l->q) ||
				    !atomic_load_explicit(&l->running,
							 memory_order_acquire))
					break;
				atomic_fetch_add_explicit(&l->q.wait_count, 1,
							  memory_order_relaxed);
				int wait_rc =
					pthread_cond_wait(&l->q.wait_cv, &l->q.wait_mu);
				if (wait_rc) {
					int expected = 0;
					(void)atomic_compare_exchange_strong_explicit(
						&l->lifecycle_error, &expected, wait_rc,
						memory_order_release, memory_order_relaxed);
					atomic_store_explicit(&l->state,
							      LOGGER_STATE_STOPPING,
							      memory_order_release);
					atomic_store_explicit(&l->running, 0,
							      memory_order_release);
					break;
				}
			}
			logger_queue_wait_disarm(&l->q);
			stop = !atomic_load_explicit(&l->running,
						     memory_order_acquire) &&
			       logger_queue_empty(&l->q);
			int unlock_rc = RELEASE_ERR(
				pthread_mutex_queue_wait, &wait_guard);
			if (unlock_rc) {
				logger_note_synchronization_error(l, unlock_rc);
				break;
			}
		}
		if (stop)
			break;
	}
	/* executor 失败可能留下未完成的 reservation。最终广播前再次检查
	 * progress_mu：等待者要么读到 sticky error，要么在互斥区内进入等待。
	 * 此处不再持有 wait_mu，也不允许嵌套实例锁；销毁仍必须 join worker。 */
	if (atomic_load_explicit(&l->lifecycle_error, memory_order_acquire)) {
		ACQUIRE(pthread_mutex_progress_checked, progress_guard)(&l->progress_mu);
		int rc = ACQUIRE_ERR(pthread_mutex_progress_checked, &progress_guard);

		/* 后续同步失败不能覆盖原错误；未取得锁时绝不广播。 */
		if (!rc) {
			(void)pthread_cond_broadcast(&l->progress_cv);
			int unlock_rc = RELEASE_ERR(
				pthread_mutex_progress, &progress_guard);
			if (unlock_rc)
				logger_note_synchronization_error(l, unlock_rc);
		} else {
			logger_note_lifecycle_error(l, -rc);
		}
	}
	logger_scope_worker_leave();
	return NULL;
}
