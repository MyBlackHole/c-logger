#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include "support.h"
#include <sys/uio.h>

enum injection {
	NORMAL,
	WRITEV_ERROR,
	WRITE_ERROR,
	SHORT_SUCCESS,
	SHORT_ERROR,
	FILE_SYNC_ERROR,
	DIR_SYNC_ERROR
};
static _Atomic int fault, calls, block_first, format_entered, release_format;
static int output_fd = -1;
static logger_t *instance;
ssize_t __real_write(int, const void *, size_t);
ssize_t __real_writev(int, const struct iovec *, int);
int __real_fsync(int);
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);

ssize_t __wrap_write(int fd, const void *p, size_t n)
{
	if (fd == output_fd && atomic_load(&fault) == WRITE_ERROR) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(fd, p, n);
}
ssize_t __wrap_writev(int fd, const struct iovec *v, int count)
{
	if (fd == output_fd) {
		int mode = atomic_load(&fault);
		if (mode == WRITEV_ERROR) {
			errno = ENOSPC;
			return -1;
		}
		if (mode == SHORT_SUCCESS || mode == SHORT_ERROR) {
			int call = atomic_fetch_add(&calls, 1);
			if (call == 0) {
				errno = EINTR;
				return -1;
			}
			if (call == 1) {
				struct iovec short_vec = v[0];
				if (short_vec.iov_len > 3)
					short_vec.iov_len = 3;
				return __real_writev(fd, &short_vec, 1);
			}
			if (mode == SHORT_ERROR) {
				errno = EIO;
				return -1;
			}
		}
	}
	return __real_writev(fd, v, count);
}
int __wrap_fsync(int fd)
{
	int mode = atomic_load(&fault);
	struct stat st;
	int matched = mode == FILE_SYNC_ERROR && fd == output_fd;
	if (mode == DIR_SYNC_ERROR && fstat(fd, &st) == 0 &&
	    S_ISDIR(st.st_mode))
		matched = 1;
	if (matched && atomic_fetch_add(&calls, 1) == 0) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	if (atomic_exchange(&block_first, 0)) {
		atomic_store(&format_entered, 1);
		wait_flag(&release_format);
	}
	return __real_logger_format_line(l, m, out, cap);
}
static uint64_t dropped(const logger_metrics_t *m)
{
	uint64_t n = 0;
	for (unsigned i = 0; i < 6; ++i)
		n += m->dropped[i];
	return n;
}

static logger_file_metrics_t file_metrics(void)
{
	logger_file_metrics_t metrics;
	CHECK(logger_get_file_metrics(instance, &metrics) == 0);
	CHECK(metrics.current_size ==
	      (uint64_t)instance->file_backend.current_size);
	CHECK(metrics.detached == instance->file_backend.detached);
	return metrics;
}
static void check_terminal(uint64_t queued, uint64_t sync, uint64_t good,
			   uint64_t bad)
{
	logger_io_metrics_t io;
	logger_get_io_metrics(instance, &io);
	CHECK(io.async_completed == queued);
	CHECK(io.sync_completed == sync);
	CHECK(io.emitted_records == good);
	CHECK(io.failed_records == bad);
	CHECK(io.async_completed + io.sync_completed ==
	      io.emitted_records + io.failed_records);
}
static void spill_policy(void)
{
	char long_message[701];
	memset(long_message, 'L', sizeof(long_message) - 1u);
	long_message[sizeof(long_message) - 1u] = '\0';

	atomic_store(&block_first, 1);
	LOGGER_INFO(instance, "io", "worker-held");
	wait_flag(&format_entered);

	for (size_t i = 0; i < LOGGER_QUEUE_SPILL_LIMIT; ++i)
		LOGGER_INFO(instance, "io", "%s", long_message);

	CHECK(logger_queue_depth(&instance->q) == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(logger_queue_depth(&instance->q) < instance->q.cap);

	LOGGER_INFO(instance, "io", "spill-drop-%s", long_message);
	LOGGER_ERROR(instance, "io", "spill-fallback-%s", long_message);

	logger_metrics_t metrics;
	logger_get_metrics(instance, &metrics);
	CHECK(metrics.enqueued == 1u + LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(metrics.sync_fallbacks == 1);
	CHECK(dropped(&metrics) == 1);
	CHECK(metrics.queue_high_watermark == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(logger_queue_spill_exhaustions(&instance->q) == 2);
	logger_diagnostics_t diagnostics;
	logger_get_diagnostics(instance, &diagnostics);
	CHECK(diagnostics.state == LOGGER_STATE_RUNNING);
	CHECK(diagnostics.async_mode && diagnostics.worker_running);
	CHECK(diagnostics.queue_capacity == LOGGER_QUEUE_SPILL_LIMIT * 2u);
	CHECK(diagnostics.queue_depth == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(diagnostics.spill_capacity == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(diagnostics.spill_in_use == LOGGER_QUEUE_SPILL_LIMIT);
	CHECK(diagnostics.spill_exhaustions == 2);
	CHECK(diagnostics.completion_backlog ==
	      1u + LOGGER_QUEUE_SPILL_LIMIT);
	CHECK((diagnostics.observation_flags &
	       LOGGER_DIAG_SPILL_EXHAUSTIONS_OBSERVED) != 0);
	CHECK((diagnostics.observation_flags & LOGGER_DIAG_DROPS_OBSERVED) != 0);
	CHECK((diagnostics.observation_flags &
	       LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED) != 0);
	CHECK((diagnostics.observation_flags &
	       LOGGER_DIAG_QUEUE_SATURATED) == 0);
	check_terminal(0, 1, 1, 0);

	atomic_store(&release_format, 1);
	CHECK(logger_flush_instance_status(instance) == 0);
	check_terminal(1u + LOGGER_QUEUE_SPILL_LIMIT, 1,
		       2u + LOGGER_QUEUE_SPILL_LIMIT, 0);
	CHECK(file_contains("out.log", "spill-fallback-"));
	CHECK(!file_contains("out.log", "spill-drop-"));
}

static void full_queue(const char *mode)
{
	int fallback = strcmp(mode, "drop") != 0;
	int failure = strcmp(mode, "fallback-error") == 0;
	atomic_store(&block_first, 1);
	LOGGER_INFO(instance, "io", "worker-held");
	wait_flag(&format_entered);
	LOGGER_INFO(instance, "io", "queue-slot-1");
	LOGGER_INFO(instance, "io", "queue-slot-2");
	if (failure)
		atomic_store(&fault, WRITE_ERROR);
	if (fallback)
		LOGGER_ERROR(instance, "io", "fallback-message");
	else
		LOGGER_INFO(instance, "io", "queue-drop");
	logger_metrics_t metrics;
	logger_get_metrics(instance, &metrics);
	CHECK(metrics.enqueued == 3);
	CHECK(metrics.sync_fallbacks == (uint64_t)fallback);
	CHECK(dropped(&metrics) == (uint64_t)!fallback);
	CHECK(metrics.queue_high_watermark <= 2);
	logger_diagnostics_t diagnostics;
	logger_get_diagnostics(instance, &diagnostics);
	CHECK(diagnostics.queue_capacity == 2);
	CHECK(diagnostics.queue_depth == 2);
	CHECK(diagnostics.completion_backlog == 3);
	CHECK((diagnostics.observation_flags & LOGGER_DIAG_QUEUE_SATURATED) !=
	      0);
	CHECK(!!(diagnostics.observation_flags &
		 LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED) == fallback);
	CHECK(!!(diagnostics.observation_flags & LOGGER_DIAG_DROPS_OBSERVED) ==
	      !fallback);
	CHECK(!!(diagnostics.observation_flags &
		 LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED) == failure);
	check_terminal(0, fallback, fallback && !failure, failure);
	atomic_store(&release_format, 1);
	int rc = logger_flush_instance_status(instance);
	CHECK(failure ? (rc == -1 && errno == ENOSPC) : rc == 0);
	check_terminal(3, fallback, 3 + (fallback && !failure), failure);
	CHECK(!file_contains("out.log", "queue-drop"));
	if (fallback && !failure)
		CHECK(file_contains("out.log", "fallback-message"));
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/logger-io-account-XXXXXX";
	enter_temp(dir);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "out.log";
	c.queue_capacity = 2;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.flush_level = LOGGER_OFF;
	if (!strcmp(argv[1], "sync"))
		c.async_mode = 0;
	if (!strcmp(argv[1], "spill-policy"))
		c.queue_capacity = LOGGER_QUEUE_SPILL_LIMIT * 2u;
	if (!strcmp(argv[1], "rotation-fsync")) {
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		c.rotation.max_file_size = 1;
	}
	instance = logger_create(&c);
	CHECK(instance);
	output_fd = instance->file_backend.fd;
	if (!strcmp(argv[1], "spill-policy")) {
		spill_policy();
	} else if (!strcmp(argv[1], "drop") || !strcmp(argv[1], "fallback") ||
		   !strcmp(argv[1], "fallback-error")) {
		full_queue(argv[1]);
	} else if (!strcmp(argv[1], "async-error")) {
		atomic_store(&fault, WRITEV_ERROR);
		LOGGER_INFO(instance, "io", "failed-message");
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == ENOSPC);
		check_terminal(1, 0, 0, 1);
		logger_file_metrics_t file = file_metrics();
		CHECK(file.write_operations == 1);
		CHECK(file.failed_write_operations == 1);
		CHECK(file.write_syscalls == 1);
		CHECK(file.failed_write_syscalls == 1);
		CHECK(!file.bytes_written);
		CHECK(file.last_write_error == ENOSPC);
		/* status flush performed a later successful file sync */
		CHECK(file.last_error == 0);
		atomic_store(&fault, NORMAL);
		LOGGER_INFO(instance, "io", "later-success");
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == ENOSPC);
		check_terminal(2, 0, 1, 1);
		file = file_metrics();
		CHECK(file.write_operations == 2);
		CHECK(file.failed_write_operations == 1);
		CHECK(file.write_syscalls == 2);
		CHECK(file.failed_write_syscalls == 1);
		CHECK(file.bytes_written > 0);
		CHECK(file.last_write_error == ENOSPC);
		CHECK(file.last_error == 0);
		CHECK(file_contains("out.log", "later-success"));
	} else if (!strcmp(argv[1], "short-ok") ||
		   !strcmp(argv[1], "short-error")) {
		int bad = !strcmp(argv[1], "short-error");
		atomic_store(&fault, bad ? SHORT_ERROR : SHORT_SUCCESS);
		LOGGER_INFO(instance, "io", "short-writev-message");
		int rc = logger_flush_instance_status(instance);
		CHECK(bad ? (rc == -1 && errno == EIO) : rc == 0);
		check_terminal(1, 0, !bad, bad);
		logger_file_metrics_t file = file_metrics();
		CHECK(file.write_operations == 1);
		CHECK(file.failed_write_operations == (uint64_t)bad);
		CHECK(file.write_syscalls == 3);
		CHECK(file.interrupted_write_syscalls == 1);
		CHECK(file.failed_write_syscalls == (uint64_t)bad);
		CHECK(file.bytes_written ==
		      (uint64_t)(bad ? 3 : file_size("out.log")));
		CHECK(file.last_write_error == (bad ? EIO : 0));
		/* status flush is the latest File operation in both cases */
		CHECK(file.last_error == 0);
		CHECK((off_t)instance->file_backend.current_size ==
		      file_size("out.log"));
		if (bad)
			CHECK(file_size("out.log") == 3);
		else
			CHECK(file_contains("out.log", "short-writev-message"));
	} else if (!strcmp(argv[1], "file-fsync") ||
		   !strcmp(argv[1], "dir-fsync")) {
		atomic_store(&fault, !strcmp(argv[1], "file-fsync") ?
					     FILE_SYNC_ERROR :
					     DIR_SYNC_ERROR);
		LOGGER_INFO(instance, "io", "output-before-sync-failure");
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == EIO);
		check_terminal(1, 0, 1,
			       0); /* fsync failure is not an extra record */
		logger_file_metrics_t file = file_metrics();
		CHECK(file.data_sync_attempts == 1);
		CHECK(file.data_sync_failures ==
		      (uint64_t)(!strcmp(argv[1], "file-fsync")));
		CHECK(file.directory_sync_attempts ==
		      (uint64_t)(!strcmp(argv[1], "dir-fsync")));
		CHECK(file.directory_sync_failures ==
		      (uint64_t)(!strcmp(argv[1], "dir-fsync")));
		CHECK(file.last_sync_error == EIO && file.last_error == EIO);
		atomic_store(&fault, NORMAL);
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == EIO);
		file = file_metrics();
		CHECK(file.data_sync_attempts == 2);
		CHECK(file.directory_sync_attempts >= 1);
		CHECK(file.last_sync_error == EIO);
		CHECK(file.last_error == 0);
	} else if (!strcmp(argv[1], "rotation-fsync")) {
		LOGGER_INFO(instance, "io", "old-segment");
		CHECK(logger_wait_for_output(instance) == 0);
		atomic_store(&fault, FILE_SYNC_ERROR);
		LOGGER_INFO(instance, "io", "must-not-rotate-after-sync-error");
		CHECK(logger_flush_instance_status(instance) == -1 &&
		      errno == EIO);
		CHECK(instance->file_backend.fd >= 0);
		check_terminal(2, 0, 1, 1);
		logger_file_metrics_t file = file_metrics();
		CHECK(file.rotation_attempts == 1);
		CHECK(!file.rotation_successes && file.rotation_failures == 1);
		CHECK(file.last_rotation_error == EIO);
		CHECK(file.last_write_error == EIO);
		CHECK(file.data_sync_failures == 1);
		CHECK(file.failed_write_operations == 1);
		CHECK(file_contains("out.log", "old-segment"));
		CHECK(!file_contains("out.log",
				     "must-not-rotate-after-sync-error"));
	} else if (!strcmp(argv[1], "sync")) {
		LOGGER_INFO(instance, "io", "sync-output");
		CHECK(logger_flush_instance_status(instance) == 0);
		check_terminal(0, 1, 1, 0);
	} else {
		CHECK(!"unknown scenario");
	}
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_diagnostics_t diagnostics;
	logger_get_metrics(instance, &metrics);
	logger_get_io_metrics(instance, &io);
	logger_get_diagnostics(instance, &diagnostics);
	CHECK(diagnostics.enqueued == metrics.enqueued);
	CHECK(diagnostics.sync_fallbacks == metrics.sync_fallbacks);
	CHECK(diagnostics.consumer_batches == metrics.consumer_batches);
	CHECK(diagnostics.consumer_records == metrics.consumer_records);
	CHECK(diagnostics.queue_high_watermark == metrics.queue_high_watermark);
	CHECK(diagnostics.dropped_records == dropped(&metrics));
	CHECK(diagnostics.async_completed == io.async_completed);
	CHECK(diagnostics.sync_completed == io.sync_completed);
	CHECK(diagnostics.emitted_records == io.emitted_records);
	CHECK(diagnostics.failed_records == io.failed_records);
	CHECK(diagnostics.first_error == io.first_error);
	CHECK(!!(diagnostics.observation_flags & LOGGER_DIAG_DROPS_OBSERVED) ==
	      (dropped(&metrics) != 0));
	CHECK(!!(diagnostics.observation_flags &
		 LOGGER_DIAG_SYNC_FALLBACKS_OBSERVED) ==
	      (metrics.sync_fallbacks != 0));
	CHECK(!!(diagnostics.observation_flags &
		 LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED) ==
	      (io.failed_records != 0 || io.first_error != 0));
	if (diagnostics.async_mode) {
		CHECK(diagnostics.queue_depth == 0);
		CHECK(diagnostics.completion_backlog == 0);
		CHECK(diagnostics.spill_in_use == 0);
	} else {
		CHECK(!diagnostics.queue_capacity && !diagnostics.queue_depth &&
		      !diagnostics.worker_running);
	}
	printf("scenario=%s enqueued=%llu dequeued=%llu completed=%llu sync=%llu emitted=%llu failed=%llu drops=%llu first_error=%d flags=0x%x\n",
	       argv[1], (unsigned long long)metrics.enqueued,
	       (unsigned long long)metrics.consumer_records,
	       (unsigned long long)io.async_completed,
	       (unsigned long long)io.sync_completed,
	       (unsigned long long)io.emitted_records,
	       (unsigned long long)io.failed_records,
	       (unsigned long long)dropped(&metrics), io.first_error,
	       diagnostics.observation_flags);
	logger_destroy(instance);
	leave_temp(dir);
	return 0;
}
