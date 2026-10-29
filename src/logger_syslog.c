#define _GNU_SOURCE
#include <stdbool.h>
#include "logger_libc.h"
#include "logger_syslog.h"
#include "logger_fd.h"
#include "logger_fork_fd.h"
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

/*
 * 本地数据报保留记录边界。短发送时绝不能模拟 write_all()：
 * 交付结果不确定，且后缀会成为新记录。
 * 此后端不等待、轮询或休眠，也不进行无界 EINTR 循环。
 */
enum { SYSLOG_SEND_ATTEMPTS = 4, SYSLOG_PACKET_MAX = 8192 };

static unsigned int priority(logger_level_t level)
{
	static const unsigned int values[] = { 7, 7, 6, 4, 3, 2 };
	return values[level]; /* 调用者验证 level */
}

static int fail(int error)
{
	errno = error ? error : EIO;
	return -1;
}

static int monotonic_ms(struct logger_monotonic_ms *out)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
		return fail(EIO);
	uint64_t sub = (uint64_t)ts.tv_nsec / 1000000u;

	if ((uint64_t)ts.tv_sec > (UINT64_MAX - sub) / 1000u)
		return fail(EOVERFLOW);
	out->value = (uint64_t)ts.tv_sec * 1000u + sub;
	return 0;
}

static bool backpressure(int error)
{
	return error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS ||
	       error == ENOMEM;
}

static bool endpoint_unavailable(int error)
{
	return error == ENOENT || error == ECONNREFUSED ||
	       error == ECONNRESET || error == ENOTCONN || error == EINTR ||
	       error == EAGAIN || error == EWOULDBLOCK;
}

static bool broken_association(int error)
{
	return error == ECONNREFUSED || error == ECONNRESET ||
	       error == ENOTCONN || error == EPIPE || error == ENOENT ||
	       error == EDESTADDRREQ || error == EIO;
}

/*
 * 长期 fd 的唯一退役入口。调用方持有后端独占访问权。
 * 先使字段失效，再消费 fork 位图并完成一次 Linux close；
 * 即使 close 报 EINTR，也绝不能重试原 fd 整数。
 */
int logger_syslog_close_status(struct logger_syslog *s)
{
	if (!s || !logger_fd_valid(&s->fd))
		return 0;
	s->metrics.connected = 0;
	/* close 在真正关闭前失效长期拥有者，保留原首错回执。 */
	int rc = logger_fd_close_status(&s->fd);

	if (rc) {
		++s->metrics.close_failures;
		s->metrics.last_close_error = -rc;
	}
	return rc;
}

void logger_syslog_close(struct logger_syslog *s)
{
	(void)logger_syslog_close_status(s);
}

static int copy_path(struct logger_syslog *s, const char *path)
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
	/*
	 * 固定相对路径的含义，使其不受宿主后续 chdir() 影响。
	 * 不使用 procfs、不绑定套接字、不修改环境、不创建目录，也不改写符号链
	 * 接。
	 */
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

/*
 * 恰好新建一个套接字并调用一次 connect。任何连接失败都丢弃候选对象；
 * 不假定出现 EINTR/EAGAIN 的套接字可用。
 */
static int connect_once(struct logger_syslog *s, bool *deferable)
{
	*deferable = false;
	++s->metrics.connect_attempts;
	struct logger_owned_fd candidate = logger_fd_own(logger_fork_socket(
		AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
	if (!logger_fd_valid(&candidate))
		goto failed;
	struct sockaddr_un address = { 0 };

	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, s->path, strlen(s->path) + 1);
	socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
				     strlen(s->path) + 1);
	if (connect(logger_fd_number(logger_fd_borrow(&candidate)),
		    (const struct sockaddr *)&address, size) != 0) {
		int error = errno ? errno : EIO;
		*deferable = endpoint_unavailable(error);
		int close_error = logger_fd_close_status(&candidate);

		if (close_error) {
			++s->metrics.close_failures;
			s->metrics.last_close_error = -close_error;
		}
		errno = error;
		goto failed;
	}
	s->fd = logger_fd_move(&candidate);
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

int logger_syslog_init(struct logger_syslog *s, const char *ident,
		       const logger_syslog_config_t *config)
{
	if (!s || !config)
		return fail(EINVAL);
	memset(s, 0, sizeof(*s));
	s->fd = logger_fd_own(-1);
	if ((config->facility & 7u) != 0 || config->facility > 23u * 8u ||
	    (unsigned int)config->startup > LOGGER_SYSLOG_START_DEFERRED ||
	    config->reconnect_interval_ms > 60000u)
		return fail(EINVAL);
	s->facility = config->facility;
	s->reconnect_interval_ms = (struct logger_duration_ms){
		.value = config->reconnect_interval_ms ?
				 config->reconnect_interval_ms :
				 1000u,
	};
	if (!ident || !*ident)
		ident = "app";
	size_t n = strnlen(ident, sizeof(s->ident));

	if (n >= sizeof(s->ident))
		return fail(ENAMETOOLONG);
	/*
	 * 保留可解析的本地标签，避免静默截断标识符，
	 * 也不允许调用者提供的换行符或分隔符伪造封装。
	 */
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)ident[i];

		if (c < 33 || c > 126 || c == ':' || c == '[' || c == ']')
			return fail(EINVAL);
	}
	memcpy(s->ident, ident, n + 1);
	if (copy_path(s, config->path) != 0)
		return -1;
#if defined(LOGGER_ENABLE_FAULT_INJECTION) && LOGGER_ENABLE_FAULT_INJECTION
	/*
	 * 仅供旧测试夹具使用。生产代码不包含此字符串或 getenv。
	 * 应用显式设置始终优先。
	 */
	const char *override = getenv("LOGGER_SYSLOG_PATH");

	if (!config->path && override && *override) {
		n = strnlen(override, sizeof(s->path));
		if (n >= sizeof(s->path))
			return fail(ENAMETOOLONG);
		memcpy(s->path, override, n + 1);
	}
#endif /* LOGGER_ENABLE_FAULT_INJECTION */
	if (monotonic_ms(&s->retry_from_ms) != 0)
		return -1;
	bool deferable;

	if (connect_once(s, &deferable) == 0)
		return 0;
	/*
	 * 显式降级启动；指标和不会自动清除的 I/O 错误状态会反映此情况
	 */
	if (config->startup == LOGGER_SYSLOG_START_DEFERRED && deferable)
		return 0;
	return -1;
}

static int record_failure(struct logger_syslog *s, int error)
{
	if (!error)
		error = EIO;
	++s->metrics.failed_records;
	if (backpressure(error))
		++s->metrics.backpressure_records;
	s->metrics.last_error = error;
	return fail(error);
}

int logger_syslog_write(struct logger_syslog *s, logger_level_t level,
			const char *line, size_t n)
{
	if (!s)
		return fail(EINVAL);
	++s->metrics.submitted_records;
	if (!line || (unsigned int)level >= LOGGER_OFF)
		return record_failure(s, EINVAL);
	/*
	 * 访问 line[n-1] 前先验证大小。不静默截断，也不将一条记录
	 * 拆分为多个数据报。普通日志记录的尺寸更小。
	 */
	if (n > SYSLOG_PACKET_MAX)
		return record_failure(s, EMSGSIZE);
	char packet[SYSLOG_PACKET_MAX];
	int prefix = logger_libc_snprintf(
		packet, sizeof(packet),
		"<%u>%s[%ld]: ", s->facility | priority(level), s->ident,
		(long)getpid());
	/*
	 * 格式化/锁错误须保留原 errno；只有确定的容量溢出
	 * 才能报告 EOVERFLOW，避免污染真实失败指标。
	 */
	if (prefix < 0)
		return record_failure(s, errno ? errno : EILSEQ);
	if ((size_t)prefix >= sizeof(packet))
		return record_failure(s, EOVERFLOW);
	if (n && line[n - 1] == '\n')
		--n;
	if (n > sizeof(packet) - (size_t)prefix)
		return record_failure(s, EMSGSIZE);
	memcpy(packet + prefix, line, n);
	size_t size = (size_t)prefix + n;

	if (!logger_fd_valid(&s->fd)) {
		struct logger_monotonic_ms now;

		if (monotonic_ms(&now) != 0)
			return record_failure(s, errno);
		if (s->retry_clock_pending ||
		    logger_monotonic_ms_before(now, s->retry_from_ms)) {
			/*
			 * 断开连接时发生时钟失败，或读数异常倒退：
			 * 重新设置冷却期，避免引发重连风暴。
			 */
			s->retry_from_ms = now;
			s->retry_clock_pending = 0;
		}
		if (!logger_monotonic_ms_elapsed(now, s->retry_from_ms,
						 s->reconnect_interval_ms)) {
			++s->metrics.cooldown_records;
			return record_failure(s, s->connection_error ?
							 s->connection_error :
							 ENOTCONN);
		}
		s->retry_from_ms = now;
		bool deferable;

		if (connect_once(s, &deferable) != 0)
			return record_failure(s, errno);
	}

	int fd = logger_fd_number(logger_fd_borrow(&s->fd));
	int error = EINTR;

	for (unsigned int i = 0; i < SYSLOG_SEND_ATTEMPTS; ++i) {
		++s->metrics.send_attempts;
		ssize_t sent =
			send(fd, packet, size, MSG_NOSIGNAL | MSG_DONTWAIT);
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
	/*
	 * 连接丢失后不重试或重放本条记录；只有后续记录可以重连。
	 * 这不会重复已成功输出到文件或 stderr 的内容。
	 */
	return record_failure(s, error);
}
