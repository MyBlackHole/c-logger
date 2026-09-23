#define _GNU_SOURCE
#include "logger_syslog.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(LOGGER_ENABLE_FAULT_INJECTION) && LOGGER_ENABLE_FAULT_INJECTION
#include <stdlib.h>
#endif

/* Local datagrams preserve record boundaries. Never emulate write_all() on a
 * short send: the delivery result is uncertain, and a suffix is a new record.
 * No wait/poll/sleep and no unbounded EINTR loop in this backend. */
#define SYSLOG_SEND_ATTEMPTS 4u
#define SYSLOG_PACKET_MAX 8192u

static unsigned priority(logger_level_t level)
{
	static const unsigned values[] = { 7, 7, 6, 4, 3, 2 };
	return values[level]; /* caller validates level */
}

static int fail(int error)
{
	errno = error ? error : EIO;
	return -1;
}

static int monotonic_ms(uint64_t *out)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
		return fail(EIO);
	uint64_t sub = (uint64_t)ts.tv_nsec / 1000000u;
	if ((uint64_t)ts.tv_sec > (UINT64_MAX - sub) / 1000u)
		return fail(EOVERFLOW);
	*out = (uint64_t)ts.tv_sec * 1000u + sub;
	return 0;
}

static int backpressure(int error)
{
	return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS ||
	       error == ENOMEM;
}

static int endpoint_unavailable(int error)
{
	return error == ENOENT || error == ECONNREFUSED ||
	       error == ECONNRESET || error == ENOTCONN || error == EINTR ||
	       error == EAGAIN || error == EWOULDBLOCK;
}

static int broken_association(int error)
{
	return error == ECONNREFUSED || error == ECONNRESET ||
	       error == ENOTCONN || error == EPIPE || error == ENOENT ||
	       error == EDESTADDRREQ || error == EIO;
}

static void close_fd(logger_syslog_t *s, int fd)
{
	/* Linux close releases fd even on EINTR. Retrying can close a reused fd. */
	if (close(fd) != 0) {
		++s->metrics.close_failures;
		s->metrics.last_close_error = errno ? errno : EIO;
	}
}

void logger_syslog_close(logger_syslog_t *s)
{
	if (!s)
		return;
	if (s->fd >= 0) {
		int fd = s->fd;
		s->fd = -1;
		s->metrics.connected = 0;
		close_fd(s, fd);
	}
}

static int copy_path(logger_syslog_t *s, const char *path)
{
	if (!path)
		path = "/dev/log";
	size_t n = strnlen(path, sizeof(s->path));
	if (n == 0)
		return fail(EINVAL);
	if (n >= sizeof(s->path))
		return fail(ENAMETOOLONG);
	if (path[0] == '/') {
		memcpy(s->path, path, n + 1);
		return 0;
	}
	/* Freeze relative path meaning across a later host chdir(). No procfs,
     * socket bind, environment mutation, directory creation or symlink rewrite. */
	char cwd[sizeof(s->path)];
	if (!getcwd(cwd, sizeof(cwd))) {
		if (errno == ERANGE)
			errno = ENAMETOOLONG;
		return -1;
	}
	size_t len = strlen(cwd);
	size_t slash = len == 1 && cwd[0] == '/' ? 0 : 1;
	if (len + slash + n >= sizeof(s->path))
		return fail(ENAMETOOLONG);
	memcpy(s->path, cwd, len);
	if (slash)
		s->path[len++] = '/';
	memcpy(s->path + len, path, n + 1);
	return 0;
}

/* Exactly one new socket and one connect call. On ANY connect failure discard
 * the candidate; no assumption that an EINTR/EAGAIN socket is usable. */
static int connect_once(logger_syslog_t *s, int *deferable)
{
	*deferable = 0;
	++s->metrics.connect_attempts;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		goto failed;
	struct sockaddr_un address = { 0 };
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, s->path, strlen(s->path) + 1);
	socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
				     strlen(s->path) + 1);
	if (connect(fd, (const struct sockaddr *)&address, size) != 0) {
		int error = errno ? errno : EIO;
		*deferable = endpoint_unavailable(error);
		close_fd(s, fd);
		errno = error;
		goto failed;
	}
	s->fd = fd;
	s->metrics.connected = 1;
	s->connection_error = 0;
	if (s->metrics.connect_attempts > 1)
		++s->metrics.reconnect_successes;
	return 0;
failed: {
	int error = errno ? errno : EIO;
	if (error == EINTR)
		++s->metrics.interrupted_calls;
	++s->metrics.connect_failures;
	s->connection_error = error;
	s->metrics.last_error = error;
	return fail(error);
}
}

int logger_syslog_init(logger_syslog_t *s, const char *ident,
		       const logger_syslog_config_t *config)
{
	if (!s || !config)
		return fail(EINVAL);
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	if ((config->facility & 7u) != 0 || config->facility > 23u * 8u ||
	    (unsigned)config->startup > LOGGER_SYSLOG_START_DEFERRED ||
	    config->reconnect_interval_ms > 60000u)
		return fail(EINVAL);
	s->facility = config->facility;
	s->reconnect_interval_ms = config->reconnect_interval_ms ?
					   config->reconnect_interval_ms :
					   1000u;
	if (!ident || !*ident)
		ident = "app";
	size_t n = strnlen(ident, sizeof(s->ident));
	if (n >= sizeof(s->ident))
		return fail(ENAMETOOLONG);
	/* Keep a parseable local tag, rather than silently truncating identifiers
     * or allowing a caller-provided newline/delimiter to forge the envelope. */
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)ident[i];
		if (c < 33 || c > 126 || c == ':' || c == '[' || c == ']')
			return fail(EINVAL);
	}
	memcpy(s->ident, ident, n + 1);
	if (copy_path(s, config->path) != 0)
		return -1;
#if defined(LOGGER_ENABLE_FAULT_INJECTION) && LOGGER_ENABLE_FAULT_INJECTION
	/* Old test fixtures only. Production does not contain this string or getenv.
     * An explicit application setting always wins. */
	const char *override = getenv("LOGGER_SYSLOG_PATH");
	if (!config->path && override && *override) {
		n = strnlen(override, sizeof(s->path));
		if (n >= sizeof(s->path))
			return fail(ENAMETOOLONG);
		memcpy(s->path, override, n + 1);
	}
#endif
	if (monotonic_ms(&s->retry_from_ms) != 0)
		return -1;
	int deferable;
	if (connect_once(s, &deferable) == 0)
		return 0;
	if (config->startup == LOGGER_SYSLOG_START_DEFERRED && deferable)
		return 0; /* explicit degraded startup; metrics and sticky I/O expose it */
	return -1;
}

static int record_failure(logger_syslog_t *s, int error)
{
	if (!error)
		error = EIO;
	++s->metrics.failed_records;
	if (backpressure(error))
		++s->metrics.backpressure_records;
	s->metrics.last_error = error;
	return fail(error);
}

int logger_syslog_write(logger_syslog_t *s, logger_level_t level,
			const char *line, size_t n)
{
	if (!s)
		return fail(EINVAL);
	++s->metrics.submitted_records;
	if (!line || (unsigned)level >= LOGGER_OFF)
		return record_failure(s, EINVAL);
	/* Validate size BEFORE indexing line[n-1]. No silent truncation; no splitting
     * of one record into datagrams. The ordinary logger record is smaller. */
	if (n > SYSLOG_PACKET_MAX)
		return record_failure(s, EMSGSIZE);
	char packet[SYSLOG_PACKET_MAX];
	int prefix = snprintf(packet, sizeof(packet),
			      "<%u>%s[%ld]: ", s->facility | priority(level),
			      s->ident, (long)getpid());
	if (prefix < 0 || (size_t)prefix >= sizeof(packet))
		return record_failure(s, EOVERFLOW);
	if (n && line[n - 1] == '\n')
		--n;
	if (n > sizeof(packet) - (size_t)prefix)
		return record_failure(s, EMSGSIZE);
	memcpy(packet + prefix, line, n);
	size_t size = (size_t)prefix + n;

	if (s->fd < 0) {
		uint64_t now;
		if (monotonic_ms(&now) != 0)
			return record_failure(s, errno);
		if (s->retry_clock_pending || now < s->retry_from_ms) {
			/* Clock failure at disconnect, or an anomalous backwards reading:
             * establish a fresh cooldown rather than opening a reconnect storm. */
			s->retry_from_ms = now;
			s->retry_clock_pending = 0;
		}
		if (now - s->retry_from_ms < s->reconnect_interval_ms) {
			++s->metrics.cooldown_records;
			return record_failure(s, s->connection_error ?
							 s->connection_error :
							 ENOTCONN);
		}
		s->retry_from_ms = now;
		int deferable;
		if (connect_once(s, &deferable) != 0)
			return record_failure(s, errno);
	}

	int error = EINTR;
	for (unsigned i = 0; i < SYSLOG_SEND_ATTEMPTS; ++i) {
		++s->metrics.send_attempts;
		ssize_t sent =
			send(s->fd, packet, size, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (sent == (ssize_t)size) {
			++s->metrics.sent_records;
			s->metrics.last_error = 0;
			return 0;
		}
		if (sent >= 0) {
			error = EIO;
			break;
		}
		error = errno ? errno : EIO;
		if (error != EINTR)
			break;
		++s->metrics.interrupted_calls;
	}
	if (broken_association(error)) {
		logger_syslog_close(s);
		s->connection_error = error;
		if (monotonic_ms(&s->retry_from_ms) != 0)
			s->retry_clock_pending = 1;
	}
	/* No retry/replay of this record after connection loss; only future records
     * may reconnect. This cannot duplicate a successful file/stderr emission. */
	return record_failure(s, error);
}
