#define _GNU_SOURCE
#include "audit_support.h"
#include "audit_internal.h"
#include "logger_internal.h"
#include <stdarg.h>

/* Link-time wrappers only: no callback/test branch is added to Audit's runtime. */
static _Atomic int cp_failures, cp_calls, armed_fd = -1, io_mode, io_calls;
static _Atomic int stop_cp_failure, start_cp_failure, offset_failure;
int __real_audit_checkpoint_persist(const char *, const audit_ckpt_t *);
int __real_logger_log_sync_status(logger_t *, logger_level_t, const char *,
				  const char *, int, const char *, const char *,
				  ...);
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_logger_file_offset(logger_t *, uint64_t *);
int __wrap_logger_file_offset(logger_t *l, uint64_t *offset)
{
	if (atomic_exchange(&offset_failure, 0)) {
		errno = EIO;
		return -1;
	}
	return __real_logger_file_offset(l, offset);
}

int __wrap_audit_checkpoint_persist(const char *path, const audit_ckpt_t *cp)
{
	atomic_fetch_add(&cp_calls, 1);
	int n = atomic_load(&cp_failures);
	if (n) {
		if (n > 0)
			atomic_fetch_sub(&cp_failures, 1);
		errno = n < 0 ? ENOSPC : EIO;
		return -1;
	}
	return __real_audit_checkpoint_persist(path, cp);
}
int __wrap_logger_log_sync_status(logger_t *l, logger_level_t level,
				  const char *module, const char *file,
				  int line, const char *func, const char *fmt,
				  ...)
{
	char text[16384];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	CHECK(n >= 0 && (size_t)n < sizeof(text));
	atomic_store(&armed_fd, l->file_backend.fd);
	if (strstr(text, "event=\"AUDIT_STOP\"") &&
	    atomic_exchange(&stop_cp_failure, 0))
		atomic_store(&cp_failures, 1);
	if (strstr(text, "event=\"AUDIT_START\"") &&
	    atomic_exchange(&start_cp_failure, 0))
		atomic_store(&cp_failures, 1);
	return __real_logger_log_sync_status(l, level, module, file, line, func,
					     "%s", text);
}
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
	if (fd == atomic_load(&armed_fd)) {
		int mode = atomic_load(&io_mode);
		if (mode == 1) {
			errno = ENOSPC;
			return -1;
		}
		if (mode == 2) {
			if (!atomic_fetch_add(&io_calls, 1))
				return __real_write(fd, p, n > 17 ? 17 : n);
			errno = EIO;
			return -1;
		}
	}
	return __real_write(fd, p, n);
}
int __wrap_fsync(int fd)
{
	if (fd == atomic_load(&armed_fd) && atomic_load(&io_mode) == 3) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

#ifndef AUDIT_BASELINE_PROBE
static audit_status_t status(void)
{
	audit_status_t st;
	CHECK(audit_get_status(&st) == 0);
	return st;
}
#endif

static void checkpoint_once(int fail_offset)
{
	audit_event_t e = audit_test_event("FIRST");
	if (fail_offset)
		atomic_store(&offset_failure, 1);
	else
		atomic_store(&cp_failures, 1);
	CHECK(audit_write(&e) == -1 && errno == EIO);
	off_t size = file_size("app.audit.log");
#ifndef AUDIT_BASELINE_PROBE
	audit_status_t st = status();
	CHECK(st.state == AUDIT_STATE_CHECKPOINT_FAILED && st.checkpoint_dirty);
	CHECK(st.committed_seq == 2 && st.checkpoint_seq == 1 &&
	      st.error_code == EIO);
	e.event = "BLOCKED";
	CHECK(audit_write(&e) == -1 && errno == EIO);
	CHECK(audit_begin(&e) == -1 && errno == EIO && !e.transaction_id);
	CHECK(file_size("app.audit.log") == size);
#endif
	CHECK(audit_flush() == 0); /* only retry checkpoint, NOT the record */
	CHECK(file_size("app.audit.log") == size);
	e.event = "AFTER";
	CHECK(audit_write(&e) == 0);
	audit_shutdown();
	CHECK(count_event("FIRST") == 1 && count_event("AFTER") == 1);
	/* This assertion fails on round1: its flush doesn't reconcile g_prev_hash. */
	CHECK(audit_verify_file("app.audit.log") == 0);
}

#ifndef AUDIT_BASELINE_PROBE
static void checkpoint_persistent(void)
{
	audit_event_t e = audit_test_event("COMMITTED");
	atomic_store(&cp_failures, -1);
	CHECK(audit_write(&e) == -1 && errno == ENOSPC);
	off_t n = file_size("app.audit.log");
	for (int i = 0; i < 3; ++i) {
		CHECK(audit_flush() == -1 && errno == ENOSPC);
		CHECK(audit_write(&e) == -1 && errno == ENOSPC);
		CHECK(file_size("app.audit.log") == n);
	}
	CHECK(audit_shutdown_status() == -1 && errno == ENOSPC);
	CHECK(status().state == AUDIT_STATE_IDLE &&
	      status().error_code == ENOSPC);
	CHECK(!count_event("AUDIT_STOP"));
	check_process_lock("free");
	atomic_store(&cp_failures, 0);
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(count_event("COMMITTED") == 1);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void uncertain(const char *mode)
{
	int expected;
	if (!strcmp(mode, "io-write")) {
		atomic_store(&io_mode, 1);
		expected = ENOSPC;
	} else if (!strcmp(mode, "io-partial")) {
		atomic_store(&io_mode, 2);
		expected = EIO;
	} else {
		atomic_store(&io_mode, 3);
		expected = EIO;
	}
	audit_event_t e = audit_test_event("UNCERTAIN");
	CHECK(audit_write(&e) == -1 && errno == expected);
	audit_status_t st = status();
	CHECK(st.state == AUDIT_STATE_IO_FAILED && st.committed_seq == 1);
	atomic_store(
		&io_mode,
		0); /* removing the syscall fault must NOT reopen admission */
	off_t size = file_size("app.audit.log");
	e.event = "FORBIDDEN";
	CHECK(audit_write(&e) == -1 && errno == expected);
	CHECK(audit_flush() == -1 && errno == expected);
	CHECK(file_size("app.audit.log") == size);
	CHECK(audit_shutdown_status() == -1 && errno == expected);
	CHECK(!count_event("AUDIT_STOP") && !count_event("FORBIDDEN"));
	check_process_lock("free");
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) ==
	      0); /* controlled current-file recovery, not arbitrary history */
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void preflight(void)
{
	audit_event_t e = audit_test_event("VALID");
	off_t size = file_size("app.audit.log");
	char too_long[6000];
	memset(too_long, 'x', sizeof(too_long) - 1);
	too_long[sizeof(too_long) - 1] = 0;
	e.detail = too_long;
	CHECK(audit_write(&e) == -1 && errno == EOVERFLOW);
	e.detail = NULL;
	e.phase = (audit_phase_t)123;
	CHECK(audit_write(&e) == -1 && errno == EINVAL);
	e.phase = AUDIT_PHASE_RESULT;
	e.result = (audit_result_t)123;
	CHECK(audit_write(&e) == -1 && errno == EINVAL);
	e.result = AUDIT_SUCCESS;
	e.event = "AUDIT_STOP";
	CHECK(audit_write(&e) == -1 && errno == EINVAL);
	CHECK(file_size("app.audit.log") == size);
	CHECK(status().state == AUDIT_STATE_RUNNING &&
	      status().committed_seq == 1);
	e.event = "VALID";
	CHECK(audit_write(&e) == 0);
	CHECK(status().committed_seq == 2);
	CHECK(audit_shutdown_status() == 0);
}

static void stop_error(void)
{
	atomic_store(&stop_cp_failure, 1);
	CHECK(audit_shutdown_status() == -1 && errno == EIO);
	audit_status_t st = status();
	CHECK(st.state == AUDIT_STATE_IDLE && st.checkpoint_dirty &&
	      st.committed_seq == 2);
	CHECK(count_event("AUDIT_STOP") == 1);
	check_process_lock("free");
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void start_error(void)
{
	audit_config_t c = audit_test_config();
	atomic_store(&start_cp_failure, 1);
	CHECK(audit_init(&c) == -1 && errno == EIO);
	audit_status_t st = status();
	CHECK(st.state == AUDIT_STATE_IDLE && st.committed_seq == 1 &&
	      st.error_code == EIO);
	audit_event_t e = audit_test_event("NO_SESSION");
	CHECK(audit_write(&e) == -1 && errno == ENODEV);
	CHECK(!count_event("AUDIT_STOP"));
	check_process_lock("free");
	CHECK(audit_init(&c) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}
#endif

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--probe-lock"))
		return audit_probe_lock(argv[2]);
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-commit-XXXXXX";
	enter_temp(dir);
#ifndef AUDIT_BASELINE_PROBE
	if (!strcmp(argv[1], "start-error")) {
		start_error();
		audit_test_cleanup(dir);
		return 0;
	}
#endif
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	if (!strcmp(argv[1], "checkpoint-once") ||
	    !strcmp(argv[1], "offset-error"))
		checkpoint_once(!strcmp(argv[1], "offset-error"));
#ifndef AUDIT_BASELINE_PROBE
	else if (!strcmp(argv[1], "checkpoint-persistent"))
		checkpoint_persistent();
	else if (!strncmp(argv[1], "io-", 3))
		uncertain(argv[1]);
	else if (!strcmp(argv[1], "preflight"))
		preflight();
	else if (!strcmp(argv[1], "stop-error"))
		stop_error();
#endif
	else
		CHECK(!"unknown mode");
	audit_test_cleanup(dir);
	return 0;
}
