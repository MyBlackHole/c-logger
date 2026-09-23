#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Syscall wrappers exist only in this test executable. No production hook. */
static int inject_socket, inject_connect, inject_send, inject_clock,
	inject_close;
static unsigned send_interrupts, sends, connects, sockets, closes;
static int send_partial, watch;
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
static void run(const char *mode, int fd)
{
	logger_config_t c = config();
	watch = 1;
	if (!strncmp(mode, "socket-", 7)) {
		inject_socket = parse_error(mode + 7);
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		CHECK(!logger_create(&c) && errno == inject_socket);
		CHECK(sockets == 1 && connects == 0);
		return;
	}
	if (!strncmp(mode, "connect-", 8)) {
		inject_connect = parse_error(mode + 8);
		CHECK(!logger_create(&c) && errno == inject_connect);
		CHECK(sockets == 1 && connects == 1 && closes == 1);
		return;
	}
	if (!strcmp(mode, "deferred-permission")) {
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		inject_connect = EACCES;
		CHECK(!logger_create(&c) && errno == EACCES);
		CHECK(closes == 1);
		return;
	}
	if (!strcmp(mode, "clock-init")) {
		inject_clock = EIO;
		CHECK(!logger_create(&c) && errno == EIO);
		CHECK(!sockets);
		return;
	}
	if (!strcmp(mode, "deferred-cooldown") ||
	    !strcmp(mode, "backwards-clock")) {
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		inject_connect = ENOENT;
		logger_t *l = logger_create(&c);
		CHECK(l);
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
		CHECK(logger_flush_instance_status(l) == -1 && errno == ENOENT);
		CHECK(logger_destroy_status(l) == -1 && errno == ENOENT);
		return;
	}
	logger_t *l = logger_create(&c);
	CHECK(l);
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
		CHECK(logger_flush_instance_status(l) == -1 && errno == error);
		CHECK(logger_destroy_status(l) == -1 && errno == error);
	} else
		CHECK(!logger_destroy_status(l));
}
int main(int argc, char **argv)
{
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
