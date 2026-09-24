#define _GNU_SOURCE
#include "logger_internal.h"
#include "logger_cleanup.h"
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
	/* Ownership transfer: lexical allocation owners -> workspace object.
	 * logger_worker_workspace_destroy() is the final releaser. */
	workspace->batch = no_free_ptr(batch);
	workspace->vec = no_free_ptr(vec);
	workspace->copy = no_free_ptr(copy);
	workspace->lines = no_free_ptr(lines);
	workspace->lens = no_free_ptr(lens);
	workspace->failed = no_free_ptr(failed);
	/* Ownership transfer: workspace constructor -> caller/logger instance. */
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


static int stderr_sigpipe_guard_begin(sigset_t *old_mask, int *was_pending)
{
	sigset_t block, pending;
	if (sigemptyset(&block) != 0 || sigaddset(&block, SIGPIPE) != 0)
		return -(errno ? errno : EINVAL);
	int rc = pthread_sigmask(SIG_BLOCK, &block, old_mask);
	if (rc != 0)
		return -rc;
	if (sigpending(&pending) != 0) {
		int error = errno ? errno : EIO;
		(void)pthread_sigmask(SIG_SETMASK, old_mask, NULL);
		return -error;
	}
	int member = sigismember(&pending, SIGPIPE);
	if (member < 0) {
		int error = errno ? errno : EINVAL;
		(void)pthread_sigmask(SIG_SETMASK, old_mask, NULL);
		return -error;
	}
	*was_pending = member;
	return 0;
}

static void stderr_sigpipe_consume_new(int was_pending)
{
	if (was_pending)
		return;
	sigset_t block, pending;
	if (sigemptyset(&block) != 0 || sigaddset(&block, SIGPIPE) != 0)
		return;
	if (sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) != 1)
		return;
	struct timespec timeout = { 0 };
	for (;;) {
		int rc = sigtimedwait(&block, NULL, &timeout);
		if (rc == SIGPIPE || (rc < 0 && errno == EAGAIN))
			return;
		if (rc < 0 && errno == EINTR)
			continue;
		return;
	}
}

static int stderr_writev_all(struct iovec *v, int count)
{
	sigset_t old_mask;
	int was_pending = 0;
	int result = stderr_sigpipe_guard_begin(&old_mask, &was_pending);
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
	if (result == -EPIPE)
		stderr_sigpipe_consume_new(was_pending);
	int restore = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	if (!result && restore)
		result = -restore;
	return result;
}

int logger_emit_status(logger_t *l, const logger_message_t *m, int force_sync)
{
	char line[LOGGER_LINE_MAX];
	size_t n = logger_format_line(l, m, line, sizeof(line));
	int rc = 0;
	guard(pthread_mutex)(&l->emit_mu);
	if (l->outputs & LOGGER_OUT_STDERR) {
		struct iovec v = { .iov_base = line, .iov_len = n };
		rc = stderr_writev_all(&v, 1);
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

	guard(pthread_mutex)(&l->emit_mu);
	if (l->outputs & LOGGER_OUT_STDERR) {
		memcpy(workspace->copy, workspace->vec,
		       count * sizeof(*workspace->vec));
		int rc = stderr_writev_all(workspace->copy, (int)count);
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

			{
				guard(pthread_mutex)(&l->progress_mu);
				atomic_fetch_add_explicit(&l->async_completed, n,
							  memory_order_release);
				l->completed_pos = atomic_load_explicit(
					&l->q.dequeue_pos, memory_order_acquire);
				pthread_cond_broadcast(&l->progress_cv);
			}
			continue;
		}
		int stop;
		{
			guard(pthread_mutex)(&l->q.wait_mu);
			while (logger_queue_empty(&l->q) &&
			       atomic_load_explicit(&l->running, memory_order_acquire))
				pthread_cond_wait(&l->q.wait_cv, &l->q.wait_mu);
			stop = !atomic_load_explicit(&l->running,
						     memory_order_acquire) &&
			       logger_queue_empty(&l->q);
		}
		if (stop)
			break;
	}
	logger_scope_worker_leave();
	return NULL;
}
