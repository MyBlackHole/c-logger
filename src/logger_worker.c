#define _GNU_SOURCE
#include "logger_internal.h"
#include <errno.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define BATCH_MAX 256

static int stderr_writev_all(struct iovec *v, int count)
{
	int first = 0;
	while (first < count) {
		ssize_t n = writev(STDERR_FILENO, v + first, count - first);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
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
	return 0;
}

int logger_emit_status(logger_t *l, const logger_message_t *m, int force_sync)
{
	char line[LOGGER_LINE_MAX];
	size_t n = logger_format_line(l, m, line, sizeof(line));
	int rc = 0;
	pthread_mutex_lock(&l->emit_mu);
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
	pthread_mutex_unlock(&l->emit_mu);
	return rc;
}

static void emit_batch(logger_t *l, const logger_message_t *msgs, size_t count)
{
	struct iovec vec[BATCH_MAX];
	char lines[BATCH_MAX][LOGGER_LINE_MAX];
	size_t lens[BATCH_MAX];
	unsigned char failed[BATCH_MAX] = { 0 };
	for (size_t i = 0; i < count; ++i) {
		lens[i] = logger_format_line(l, &msgs[i], lines[i],
					     sizeof(lines[i]));
		vec[i].iov_base = lines[i];
		vec[i].iov_len = lens[i];
	}

	pthread_mutex_lock(&l->emit_mu);
	if (l->outputs & LOGGER_OUT_STDERR) {
		struct iovec copy[BATCH_MAX];
		memcpy(copy, vec, count * sizeof(*vec));
		int rc = stderr_writev_all(copy, (int)count);
		if (rc < 0) {
			/* A partial batch does not have a per-record acknowledgement.
             * Conservatively classify all its records as unconfirmed. */
			memset(failed, 1, count);
			logger_note_io_error(l, -rc);
		}
	}
	if (l->outputs & LOGGER_OUT_FILE) {
		size_t total = 0;
		int need_sync = 0;
		for (size_t i = 0; i < count; ++i) {
			total += lens[i];
			if (msgs[i].level >= l->flush_level)
				need_sync = 1;
		}
		struct iovec copy[BATCH_MAX];
		memcpy(copy, vec, count * sizeof(*vec));
		int rc = logger_file_writev(&l->file_backend, copy, (int)count,
					    total, &msgs[count - 1].ts,
					    need_sync);
		if (rc < 0) {
			memset(failed, 1, count);
			logger_note_io_error(l, -rc);
		}
	}
	if (l->outputs & LOGGER_OUT_SYSLOG) {
		for (size_t i = 0; i < count; ++i) {
			if (logger_syslog_write(&l->syslog_backend,
						msgs[i].level, lines[i],
						lens[i]) != 0) {
				failed[i] = 1;
				logger_note_io_error(l, errno ? errno : EIO);
			}
		}
	}
	uint64_t bad = 0;
	for (size_t i = 0; i < count; ++i)
		bad += failed[i];
	atomic_fetch_add_explicit(&l->failed_records, bad,
				  memory_order_relaxed);
	atomic_fetch_add_explicit(&l->emitted_records, count - bad,
				  memory_order_relaxed);
	pthread_mutex_unlock(&l->emit_mu);
}

void *logger_worker_main(void *p)
{
	logger_t *l = p;
	logger_scope_worker_enter();
	logger_message_t batch[BATCH_MAX];
	for (;;) {
		size_t n = logger_queue_drain(&l->q, batch, BATCH_MAX);
		if (n != 0) {
			/* Retain legacy metrics as dequeue statistics. Completion below
             * is a DIFFERENT event and is also advanced on output failure. */
			atomic_fetch_add_explicit(&l->consumer_batches, 1,
						  memory_order_relaxed);
			atomic_fetch_add_explicit(&l->consumer_records, n,
						  memory_order_relaxed);
			emit_batch(l, batch, n);

			pthread_mutex_lock(&l->progress_mu);
			atomic_fetch_add_explicit(&l->async_completed, n,
						  memory_order_release);
			l->completed_pos = atomic_load_explicit(
				&l->q.dequeue_pos, memory_order_acquire);
			pthread_cond_broadcast(&l->progress_cv);
			pthread_mutex_unlock(&l->progress_mu);
			continue;
		}
		pthread_mutex_lock(&l->q.wait_mu);
		while (logger_queue_empty(&l->q) &&
		       atomic_load_explicit(&l->running, memory_order_acquire))
			pthread_cond_wait(&l->q.wait_cv, &l->q.wait_mu);
		int stop = !atomic_load_explicit(&l->running,
						 memory_order_acquire) &&
			   logger_queue_empty(&l->q);
		pthread_mutex_unlock(&l->q.wait_mu);
		if (stop)
			break;
	}
	logger_scope_worker_leave();
	return NULL;
}
