#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

enum query {
	Q_STATE,
	Q_DROPPED,
	Q_METRICS,
	Q_IO,
	Q_DIAGNOSTICS,
	Q_FILE,
	Q_SYSLOG,
	Q_DEFAULT_DROPPED,
	Q_DEFAULT_METRICS,
	Q_DEFAULT_IO,
	Q_DEFAULT_DIAGNOSTICS,
	Q_DEFAULT_FILE,
	Q_COUNT
};

union query_value {
	logger_state_t state;
	uint64_t dropped;
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_diagnostics_t diagnostics;
	logger_file_metrics_t file;
	logger_syslog_metrics_t syslog;
};

struct query_slot {
	unsigned char before[16];
	union query_value value;
	unsigned char after[16];
};

static unsigned int checks;

static int query(enum query op, logger_t *owner, union query_value *out)
{
	switch (op) {
	case Q_STATE:
		return logger_get_state(owner, out ? &out->state : NULL);
	case Q_DROPPED:
		return logger_dropped(owner, out ? &out->dropped : NULL);
	case Q_METRICS:
		return logger_get_metrics(owner, out ? &out->metrics : NULL);
	case Q_IO:
		return logger_get_io_metrics(owner, out ? &out->io : NULL);
	case Q_DIAGNOSTICS:
		return logger_get_diagnostics(owner,
					      out ? &out->diagnostics : NULL);
	case Q_FILE:
		return logger_get_file_metrics(owner, out ? &out->file : NULL);
	case Q_SYSLOG:
		return logger_get_syslog_metrics(owner,
						 out ? &out->syslog : NULL);
	case Q_DEFAULT_DROPPED:
		return logger_global_dropped(out ? &out->dropped : NULL);
	case Q_DEFAULT_METRICS:
		return logger_get_global_metrics(out ? &out->metrics : NULL);
	case Q_DEFAULT_IO:
		return logger_get_global_io_metrics(out ? &out->io : NULL);
	case Q_DEFAULT_DIAGNOSTICS:
		return logger_get_global_diagnostics(out ? &out->diagnostics :
							   NULL);
	case Q_DEFAULT_FILE:
		return logger_get_global_file_metrics(out ? &out->file : NULL);
	default:
		CHECK(0);
		return -EINVAL;
	}
}

static void rejected(enum query op, logger_t *owner, int error)
{
	struct query_slot slot;
	unsigned char before[sizeof(slot)];

	memset(&slot, 0xa5, sizeof(slot));
	memcpy(before, &slot, sizeof(before));
	errno = EDOM;
	CHECK(query(op, owner, &slot.value) == -error && errno == EDOM);
	/* 包括结构尾部、未选联合成员及两侧哨兵，不能只检查首字节。 */
	CHECK(!memcmp(before, &slot, sizeof(slot)));
	++checks;
}

static void null_output(enum query op, logger_t *owner, int error)
{
	errno = EDOM;
	CHECK(query(op, owner, NULL) == -error && errno == EDOM);
	++checks;
}

static void admission_rejected(logger_t *owner, int error)
{
	for (enum query op = Q_STATE; op < Q_COUNT; ++op) {
		rejected(op, owner, error);
		null_output(op, owner, error);
	}
}

static logger_config_t config(int async)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = async;
	cfg.queue_capacity = 64;
	return cfg;
}

static void succeeded(enum query op, logger_t *owner, int async,
		      unsigned int records, unsigned int outputs)
{
	struct query_slot slot;
	union query_value *out = &slot.value;

	memset(&slot, 0xa5, sizeof(slot));
	errno = EDOM;
	CHECK(query(op, owner, out) == 0 && errno == EDOM);
	for (size_t i = 0; i < sizeof(slot.before); ++i)
		CHECK(slot.before[i] == 0xa5 && slot.after[i] == 0xa5);
	switch (op) {
	case Q_STATE:
		CHECK(out->state == LOGGER_STATE_RUNNING);
		break;
	case Q_DROPPED:
	case Q_DEFAULT_DROPPED:
		CHECK(out->dropped == 0);
		break;
	case Q_METRICS:
	case Q_DEFAULT_METRICS:
		for (size_t i = 0; i < 6; ++i)
			CHECK(out->metrics.dropped[i] == 0);
		CHECK(out->metrics.enqueued == (async ? records : 0));
		CHECK(out->metrics.sync_fallbacks == 0);
		CHECK(out->metrics.consumer_records == (async ? records : 0));
		break;
	case Q_IO:
	case Q_DEFAULT_IO:
		CHECK(out->io.emitted_records == records);
		CHECK(out->io.failed_records == 0 && out->io.first_error == 0);
		CHECK(out->io.async_completed == (async ? records : 0));
		CHECK(out->io.sync_completed == (async ? 0 : records));
		break;
	case Q_DIAGNOSTICS:
	case Q_DEFAULT_DIAGNOSTICS:
		CHECK(out->diagnostics.state == LOGGER_STATE_RUNNING);
		CHECK(out->diagnostics.outputs == outputs);
		CHECK(out->diagnostics.async_mode == async);
		CHECK(out->diagnostics.emitted_records == records);
		CHECK(out->diagnostics.enqueued == (async ? records : 0));
		CHECK(out->diagnostics.queue_capacity == (async ? 64u : 0u));
		CHECK(out->diagnostics.completion_backlog == 0);
		CHECK(out->diagnostics.first_error == 0);
		break;
	case Q_FILE:
	case Q_DEFAULT_FILE:
		CHECK(out->file.write_operations > 0);
		CHECK(out->file.failed_write_operations == 0);
		CHECK(out->file.bytes_written ==
		      (uint64_t)file_size("out.log"));
		CHECK(out->file.current_size == out->file.bytes_written);
		CHECK(out->file.last_error == 0);
		break;
	case Q_SYSLOG:
		CHECK(out->syslog.connected == 1);
		CHECK(out->syslog.submitted_records == records);
		CHECK(out->syslog.sent_records == records);
		CHECK(out->syslog.failed_records == 0);
		CHECK(out->syslog.connect_attempts == 1);
		CHECK(out->syslog.connect_failures == 0);
		break;
	default:
		CHECK(0);
	}
	++checks;
}

static int receiver(void)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };

	CHECK(fd >= 0);
	memcpy(address.sun_path, "syslog.sock", sizeof("syslog.sock"));
	CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static void ready(int async, int syslog)
{
	CHECK(logger_process_init() == 0);
	for (enum query op = Q_STATE; op < Q_DEFAULT_DROPPED; ++op)
		rejected(op, NULL, EINVAL);
	for (enum query op = Q_STATE; op < Q_COUNT; ++op)
		null_output(op, NULL, EINVAL);
	for (enum query op = Q_DEFAULT_DROPPED; op < Q_COUNT; ++op)
		rejected(op, NULL, ENODEV);

	int fd = syslog ? receiver() : -1;
	logger_config_t cfg = config(async);
	logger_t *owner = NULL;
	unsigned int records = syslog ? 1u : 4u;

	if (syslog) {
		cfg.outputs |= LOGGER_OUT_SYSLOG;
		cfg.syslog.path = "syslog.sock";
	}
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	for (unsigned int i = 0; i < records; ++i)
		CHECK(LOGGER_INFO(owner, "query", "record=%u", i) == 0);
	CHECK(logger_flush(owner) == 0);
	for (enum query op = Q_STATE; op < Q_COUNT; ++op) {
		null_output(op, owner, EINVAL);
		if (op == Q_SYSLOG && !syslog)
			rejected(op, owner, ENOTSUP);
		else
			succeeded(op, owner, async, records, cfg.outputs);
	}
	if (syslog) {
		char packet[1024];

		CHECK(recv(fd, packet, sizeof(packet), MSG_DONTWAIT) > 0);
		CHECK(recv(fd, packet, sizeof(packet), MSG_DONTWAIT) == -1);
		CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
	}
	CHECK(logger_default_set(owner, NULL) == 0);
	for (enum query op = Q_DEFAULT_DROPPED; op < Q_COUNT; ++op)
		rejected(op, NULL, ENODEV);
	CHECK(logger_destroy(&owner) == 0 && owner == NULL);
	if (syslog) {
		CHECK(close(fd) == 0);
		CHECK(unlink("syslog.sock") == 0);
	}

	/* 未选择后端的实例仍可查询其他指标，不能误报空默认槽。 */
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.async_mode = 0;
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	rejected(Q_FILE, owner, ENOTSUP);
	rejected(Q_SYSLOG, owner, ENOTSUP);
	rejected(Q_DEFAULT_FILE, owner, ENOTSUP);
	CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(logger_destroy(&owner) == 0 && owner == NULL);
}

static void fork_rejected(void)
{
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = config(0);
	logger_t *owner = NULL;

	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	CHECK(LOGGER_INFO(owner, "query", "parent-record") == 0);
	pid_t pid = fork();

	CHECK(pid >= 0);
	if (!pid) {
		/* 原实例地址已被 DONTFORK 隔离，查询必须在解引用前拒绝。 */
		admission_rejected(owner, ECHILD);
		_exit(0);
	}
	int status;
	pid_t got;

	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	for (enum query op = Q_STATE; op < Q_COUNT; ++op) {
		if (op == Q_SYSLOG)
			rejected(op, owner, ENOTSUP);
		else
			succeeded(op, owner, 0, 1, cfg.outputs);
	}
	CHECK(logger_default_set(owner, NULL) == 0);
	CHECK(logger_destroy(&owner) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char path[] = "/tmp/logger-query-api-XXXXXX";

	enter_temp(path);
	if (!strcmp(argv[1], "cold")) {
		admission_rejected((logger_t *)(uintptr_t)1, ENODEV);
		CHECK(access("out.log", F_OK) == -1 && errno == ENOENT);
		CHECK(chdir("/") == 0 && rmdir(path) == 0);
	} else {
		if (!strcmp(argv[1], "fork"))
			fork_rejected();
		else {
			CHECK(!strcmp(argv[1], "sync") ||
			      !strcmp(argv[1], "async") ||
			      !strcmp(argv[1], "syslog"));
			ready(!strcmp(argv[1], "async"),
			      !strcmp(argv[1], "syslog"));
		}
		leave_temp(path);
	}
	printf("QUERY_API_PASS=%s checks=%u\n", argv[1], checks);
	return 0;
}
