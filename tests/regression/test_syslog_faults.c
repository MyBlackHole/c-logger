#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_fork_fd.h"
#include "logger_syslog.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

/* 系统调用包装函数只存在于此测试可执行文件。不含生产钩子。 */
static int inject_socket, inject_connect, inject_send, inject_clock,
	inject_close;
static unsigned send_interrupts, sends, connects, sockets, closes;
static int send_partial, watch, reuse_during_close;
static int reused_fd = -1;
static uint64_t now_ms = 10000;
static char endpoint[108], temp[] = "/tmp/lg-sys-fault-XXXXXX";

int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol)
{
	if (watch && domain == AF_UNIX) {
		++sockets;
		CHECK(type & SOCK_NONBLOCK);
		CHECK(type & SOCK_CLOEXEC);
		if (inject_socket) {
			errno = inject_socket;
			return -1;
		}
	}
	return __real_socket(domain, type, protocol);
}
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t n)
{
	if (watch) {
		++connects;
		CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
		CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
		const struct sockaddr_un *a = (const struct sockaddr_un *)addr;
		CHECK(a->sun_family == AF_UNIX &&
		      !strcmp(a->sun_path, endpoint));
		CHECK(n == offsetof(struct sockaddr_un, sun_path) +
				   strlen(endpoint) + 1);
		if (inject_connect) {
			errno = inject_connect;
			return -1;
		}
	}
	return __real_connect(fd, addr, n);
}
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __wrap_send(int fd, const void *buf, size_t n, int flags)
{
	if (watch) {
		++sends;
		CHECK(flags & MSG_DONTWAIT);
		CHECK(flags & MSG_NOSIGNAL);
		if (send_interrupts) {
			--send_interrupts;
			errno = EINTR;
			return -1;
		}
		if (inject_send) {
			errno = inject_send;
			return -1;
		}
		if (send_partial)
			return send_partial == 1 ? 0 : (ssize_t)(n - 1);
	}
	return __real_send(fd, buf, n, flags);
}
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *out)
{
	if (watch && id == CLOCK_MONOTONIC) {
		if (inject_clock) {
			errno = inject_clock;
			return -1;
		}
		out->tv_sec = (time_t)(now_ms / 1000);
		out->tv_nsec = (long)(now_ms % 1000) * 1000000L;
		return 0;
	}
	return __real_clock_gettime(id, out);
}
int __real_close(int);
int __wrap_close(int fd)
{
	if (watch)
		++closes;
	int rc = __real_close(fd);
	if (watch && inject_close) {
		if (reuse_during_close) {
			/* 真正 close 已消费旧整数；马上以外部 socket 占用该编号。 */
			CHECK(reused_fd == -1);
			int flags = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
			int replacement = __real_socket(AF_UNIX, flags, 0);
			CHECK(replacement >= 0);
			if (replacement != fd) {
				CHECK(dup2(replacement, fd) == fd);
				CHECK(!__real_close(replacement));
			}
			reused_fd = fd;
		}
		errno = inject_close;
		return -1;
	}
	return rc;
}
static int receiver(void)
{
	int fd = __real_socket(AF_UNIX,
			       SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, endpoint);
	CHECK(!bind(fd, (struct sockaddr *)&a, sizeof(a)));
	return fd;
}
static logger_config_t config(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.async_mode = 0;
	c.outputs = LOGGER_OUT_SYSLOG;
	c.syslog.path = endpoint;
	c.syslog.reconnect_interval_ms = 100;
	return c;
}
static int output(logger_t *l)
{
	return logger_log_sync_status(l, LOGGER_INFO, "fault", NULL, 0, NULL,
				      "marker");
}
static logger_syslog_metrics_t metrics(logger_t *l)
{
	logger_syslog_metrics_t m;
	CHECK(!logger_get_syslog_metrics(l, &m));
	return m;
}
static int parse_error(const char *s)
{
	struct {
		const char *name;
		int e;
	} errors[] = { { "emfile", EMFILE },
		       { "enfile", ENFILE },
		       { "enomem", ENOMEM },
		       { "eintr", EINTR },
		       { "eacces", EACCES },
		       { "eperm", EPERM },
		       { "enoent", ENOENT },
		       { "refused", ECONNREFUSED },
		       { "inprogress", EINPROGRESS },
		       { "again", EAGAIN },
		       { "prototype", EPROTOTYPE },
		       { "nobufs", ENOBUFS },
		       { "msgsize", EMSGSIZE },
		       { "pipe", EPIPE },
		       { "reset", ECONNRESET },
		       { "notconn", ENOTCONN },
		       { "io", EIO } };
	for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
		if (!strcmp(s, errors[i].name))
			return errors[i].e;
	CHECK(!"bad test error");
	return 0;
}
static int syslog_fd(const logger_syslog_t *s)
{
	return logger_fd_number(logger_fd_borrow(&s->fd));
}

/* 检查真实 Syslog 候选的所有权交接，不绕开生产 fork 资源登记。 */
static void candidate_ownership(const char *mode)
{
	logger_config_t c = config();

	if (!strcmp(mode, "candidate-fd-zero")) {
		/* stdin 可能已由 CI 关闭；在两种情况下都能恢复原有状态。 */
		int saved_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
		CHECK(saved_stdin >= 0 || errno == EBADF);
		if (saved_stdin >= 0)
			CHECK(!__real_close(STDIN_FILENO));

		logger_syslog_t syslog;
		CHECK(!logger_syslog_init(&syslog, "app", &c.syslog));
		CHECK(syslog_fd(&syslog) == STDIN_FILENO &&
		      syslog.metrics.connected);
		CHECK(sockets == 1 && connects == 1 && closes == 0);
		logger_syslog_close(&syslog);
		logger_syslog_close(&syslog);
		CHECK(!logger_fd_valid(&syslog.fd) &&
		      !syslog.metrics.connected);
		CHECK(closes == 1);
		CHECK(fcntl(STDIN_FILENO, F_GETFD) == -1 && errno == EBADF);
		if (saved_stdin >= 0) {
			CHECK(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
			CHECK(!__real_close(saved_stdin));
		}
		return;
	}

	int closing_error = EIO;
	if (!strcmp(mode, "candidate-eintr-reuse"))
		closing_error = EINTR;
	int primary_error = closing_error == EINTR ? EINTR : ENOENT;
	inject_connect = primary_error;
	inject_close = closing_error;
	reuse_during_close = 1;

	logger_syslog_t failed;
	CHECK(logger_syslog_init(&failed, "app", &c.syslog) == -1 &&
	      errno == primary_error);
	CHECK(!logger_fd_valid(&failed.fd) && !failed.metrics.connected);
	CHECK(failed.connection_error == primary_error);
	CHECK(failed.metrics.connect_attempts == 1 &&
	      failed.metrics.connect_failures == 1);
	CHECK(failed.metrics.close_failures == 1 &&
	      failed.metrics.last_close_error == closing_error);
	CHECK(failed.metrics.last_error == primary_error);
	CHECK(failed.metrics.interrupted_calls ==
	      (unsigned)(primary_error == EINTR));
	CHECK(sockets == 1 && connects == 1 && closes == 1);
	CHECK(reused_fd >= 0);
	int stale_fd = reused_fd;
	logger_syslog_close(&failed);
	CHECK(closes == 1 && fcntl(stale_fd, F_GETFD) >= 0);

	/* 位图已注销：旧所有者绝不能关闭宿主复用的同一整数。 */
	CHECK(logger_fork_close(stale_fd) == -1 && errno == EBADF);
	CHECK(fcntl(stale_fd, F_GETFD) >= 0);
	CHECK(!__real_close(stale_fd));
	reused_fd = -1;
	inject_connect = 0;
	inject_close = 0;
	reuse_during_close = 0;

	/* 再由真正生产入口取得原整数，必须可以重新登记并正常关闭。 */
	logger_syslog_t next;
	CHECK(!logger_syslog_init(&next, "app", &c.syslog));
	CHECK(syslog_fd(&next) == stale_fd && next.metrics.connected);
	logger_syslog_close(&next);
	CHECK(!logger_fd_valid(&next.fd) && !next.metrics.connected);
	CHECK(sockets == 2 && connects == 2 && closes == 2);
	CHECK(fcntl(stale_fd, F_GETFD) == -1 && errno == EBADF);
}

/* 两条长期 fd 退役路径必须先失效并只注销一次 fork 登记。 */
static void retire_ownership(const char *mode)
{
	logger_config_t c = config();
	int error = strstr(mode, "eintr") ? EINTR : EIO;
	unsigned before = closes;
	inject_close = error;
	reuse_during_close = 1;
	int raw;

	if (!strncmp(mode, "retire-status-", 14)) {
		logger_syslog_t syslog;
		CHECK(!logger_syslog_init(&syslog, "app", &c.syslog));
		raw = syslog_fd(&syslog);
		CHECK(raw >= 0 && syslog.metrics.connected);
		CHECK(logger_syslog_close_status(&syslog) == -error);
		CHECK(!logger_fd_valid(&syslog.fd) &&
		      !syslog.metrics.connected);
		CHECK(syslog.metrics.close_failures == 1 &&
		      syslog.metrics.last_close_error == error);
		CHECK(logger_syslog_close_status(&syslog) == 0);
		CHECK(syslog.metrics.close_failures == 1);
	} else {
		logger_t *owner = NULL;

		CHECK(logger_create(&c, &owner) == 0 && owner);
		raw = syslog_fd(&owner->syslog_backend);
		CHECK(raw >= 0);
		/* 真实最终释放：错误可回报，已消费的指针不可重试。 */
		CHECK(logger_destroy(&owner) == -error && owner == NULL);
	}
	CHECK(reused_fd == raw && closes == before + 1);
	CHECK(fcntl(raw, F_GETFD) >= 0);
	/* 位图已注销；宿主重用该数字绝不能被旧所有者第二次关闭。 */
	CHECK(logger_fork_close(raw) == -1 && errno == EBADF);
	CHECK(fcntl(raw, F_GETFD) >= 0);
	CHECK(!__real_close(raw));
	reused_fd = -1;
	inject_close = 0;
	reuse_during_close = 0;
}

static void run(const char *mode, int fd)
{
	logger_config_t c = config();
	watch = 1;
	if (!strcmp(mode, "candidate-fd-zero") ||
	    !strcmp(mode, "candidate-eintr-reuse") ||
	    !strcmp(mode, "candidate-eio-reuse")) {
		candidate_ownership(mode);
		return;
	}
	if (!strcmp(mode, "retire-status-eintr") ||
	    !strcmp(mode, "retire-status-eio") ||
	    !strcmp(mode, "retire-destroy-eintr") ||
	    !strcmp(mode, "retire-destroy-eio")) {
		retire_ownership(mode);
		return;
	}
	if (!strncmp(mode, "socket-", 7)) {
		inject_socket = parse_error(mode + 7);
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		CHECK(logger_create(&c, &(logger_t *){ NULL }) ==
		      -inject_socket);
		CHECK(sockets == 1 && connects == 0);
		return;
	}
	if (!strncmp(mode, "connect-", 8)) {
		inject_connect = parse_error(mode + 8);
		CHECK(logger_create(&c, &(logger_t *){ NULL }) ==
		      -inject_connect);
		CHECK(sockets == 1 && connects == 1 && closes == 1);
		return;
	}
	if (!strcmp(mode, "deferred-permission")) {
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		inject_connect = EACCES;
		CHECK(logger_create(&c, &(logger_t *){ NULL }) == -EACCES);
		CHECK(closes == 1);
		return;
	}
	if (!strcmp(mode, "clock-init")) {
		inject_clock = EIO;
		CHECK(logger_create(&c, &(logger_t *){ NULL }) == -EIO);
		CHECK(!sockets);
		return;
	}
	if (!strcmp(mode, "deferred-cooldown") ||
	    !strcmp(mode, "backwards-clock")) {
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		inject_connect = ENOENT;
		logger_t *l = NULL;
		CHECK(logger_create(&c, &l) == 0);
		CHECK(l != NULL);
		CHECK(sockets == 1 && connects == 1 && closes == 1);
		for (unsigned i = 0; i < 1000; ++i)
			CHECK(output(l) == -ENOENT);
		CHECK(connects == 1 && sockets == 1 && !sends);
		now_ms += 99;
		CHECK(output(l) == -ENOENT);
		CHECK(connects == 1);
		if (!strcmp(mode, "backwards-clock")) {
			now_ms = 100;
			CHECK(output(l) == -ENOENT);
			CHECK(connects == 1);
			now_ms = 199;
			CHECK(output(l) == -ENOENT);
			CHECK(connects == 1);
			now_ms = 200;
		} else
			++now_ms;
		CHECK(output(l) == -ENOENT);
		CHECK(connects == 2 && sockets == 2);
		inject_connect = 0;
		now_ms += 100;
		CHECK(!output(l));
		CHECK(connects == 3 && sockets == 3 && sends == 1);
		logger_syslog_metrics_t m = metrics(l);
		CHECK(m.reconnect_successes == 1 && m.connected &&
		      !m.last_error);
		CHECK(logger_flush(l) == -ENOENT);
		CHECK(logger_destroy(&l) == -ENOENT);
		return;
	}
	logger_t *l = NULL;
	CHECK(logger_create(&c, &l) == 0);
	CHECK(l != NULL);
	CHECK(connects == 1 && sockets == 1);
	int error = 0;
	if (!strncmp(mode, "send-", 5)) {
		error = inject_send = parse_error(mode + 5);
		CHECK(output(l) == -error);
		CHECK(sends == (error == EINTR ? 4u : 1u));
		logger_syslog_metrics_t m = metrics(l);
		CHECK(m.failed_records == 1 && m.sent_records == 0);
		if (error == EAGAIN || error == ENOBUFS || error == ENOMEM)
			CHECK(m.backpressure_records == 1 && m.connected);
		if (error == EINTR)
			CHECK(m.interrupted_calls == 4 && m.connected);
		if (error == EPIPE || error == ECONNRESET ||
		    error == ENOTCONN || error == ECONNREFUSED ||
		    error == EIO) {
			CHECK(!m.connected && closes == 1);
			for (unsigned i = 0; i < 300; ++i)
				CHECK(output(l) == -error);
			CHECK(connects == 1 && sends == 1);
			now_ms += 100;
		}
		inject_send = 0;
		CHECK(!output(l));
		CHECK(metrics(l).last_error == 0);
	} else if (!strcmp(mode, "eintr-then-success")) {
		send_interrupts = 3;
		CHECK(!output(l));
		CHECK(sends == 4);
		CHECK(metrics(l).sent_records == 1 &&
		      metrics(l).interrupted_calls == 3);
	} else if (!strcmp(mode, "zero-send") || !strcmp(mode, "short-send")) {
		send_partial = !strcmp(mode, "zero-send") ? 1 : 2;
		CHECK(output(l) == -EIO);
		error = EIO;
		CHECK(sends == 1 && closes == 1);
		CHECK(!metrics(l).connected && metrics(l).sent_records == 0);
	} else if (!strcmp(mode, "clock-disconnect")) {
		inject_send = ECONNREFUSED;
		inject_clock = EIO;
		CHECK(output(l) == -ECONNREFUSED);
		error = ECONNREFUSED;
		CHECK(output(l) == -EIO);
		CHECK(connects == 1);
		inject_send = 0;
		inject_clock = 0;
		now_ms += 1000;
		CHECK(output(l) == -ECONNREFUSED);
		CHECK(connects == 1);
		now_ms += 100;
		CHECK(!output(l));
		CHECK(connects == 2);
	} else if (!strcmp(mode, "close-disconnect")) {
		inject_send = ECONNREFUSED;
		inject_close = EIO;
		CHECK(output(l) == -ECONNREFUSED);
		error = ECONNREFUSED;
		logger_syslog_metrics_t m = metrics(l);
		CHECK(m.close_failures == 1 && m.last_close_error == EIO &&
		      !m.connected);
		inject_send = 0;
		inject_close = 0;
		now_ms += 100;
		CHECK(!output(l));
		CHECK(metrics(l).close_failures == 1);
	} else if (!strcmp(mode, "reconnect-failure")) {
		inject_send = ECONNREFUSED;
		CHECK(output(l) == -ECONNREFUSED);
		error = ECONNREFUSED;
		inject_send = 0;
		inject_connect = EACCES;
		now_ms += 100;
		CHECK(output(l) == -EACCES);
		CHECK(sockets == 2 && connects == 2);
		for (unsigned i = 0; i < 1000; ++i)
			CHECK(output(l) == -EACCES);
		CHECK(sockets == 2 && connects == 2);
		inject_connect = 0;
		now_ms += 100;
		CHECK(!output(l));
		CHECK(connects == 3);
	} else if (!strcmp(mode, "oversize")) {
		char b[9000];
		memset(b, 'x', sizeof(b));
		CHECK(logger_syslog_write(&l->syslog_backend, LOGGER_INFO, b,
					  sizeof(b)) == -1 &&
		      errno == EMSGSIZE);
		CHECK(logger_syslog_write(&l->syslog_backend, LOGGER_INFO, b,
					  SIZE_MAX) == -1 &&
		      errno == EMSGSIZE);
		CHECK(!sends);
		CHECK(metrics(l).failed_records == 2);
	} else if (!strcmp(mode, "max-packet")) {
		char b[8192];
		memset(b, 'x', sizeof(b));
		int len = snprintf(NULL, 0, "<14>app[%ld]: ", (long)getpid());
		CHECK(len > 0);
		CHECK(!logger_syslog_write(&l->syslog_backend, LOGGER_INFO, b,
					   sizeof(b) - (size_t)len));
		char rx[9000];
		ssize_t n = recv(fd, rx, sizeof(rx), MSG_DONTWAIT);
		CHECK(n == 8192);
		CHECK(logger_syslog_write(&l->syslog_backend, LOGGER_INFO, b,
					  sizeof(b) - (size_t)len + 1) == -1 &&
		      errno == EMSGSIZE);
		CHECK(sends == 1);
	} else
		CHECK(!"unknown mode");
	if (error) {
		CHECK(logger_flush(l) == -error);
		CHECK(logger_destroy(&l) == -error);
	} else
		CHECK(!logger_destroy(&l));
}
int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	CHECK(mkdtemp(temp));
	CHECK(!chdir(temp));
	CHECK(snprintf(endpoint, sizeof(endpoint), "%s/sink", temp) > 0);
	int fd = receiver();
	run(argv[1], fd);
	watch = 0;
	CHECK(!close(fd));
	CHECK(!unlink(endpoint));
	CHECK(!chdir("/"));
	CHECK(!rmdir(temp));
	printf("syslog fault scenario passed: %s\n", argv[1]);
	return 0;
}
