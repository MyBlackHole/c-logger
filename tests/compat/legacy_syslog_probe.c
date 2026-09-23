#define _GNU_SOURCE
#include "logger_v1.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define ASSERT(x)                                                        \
	do {                                                             \
		if (!(x)) {                                              \
			fprintf(stderr, "line=%d failed: %s errno=%d\n", \
				__LINE__, #x, errno);                    \
			exit(1);                                         \
		}                                                        \
	} while (0)
static char endpoint[108];
static int client, interrupt;
static unsigned sends;
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t n)
{
	(void)addr;
	(void)n;
	client = fd;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, endpoint);
	return __real_connect(fd, (struct sockaddr *)&a, sizeof(a));
}
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __wrap_send(int fd, const void *b, size_t n, int flags)
{
	if (interrupt) {
		++sends;
		errno = sends < 64 ? EINTR : EIO;
		return -1;
	}
	return __real_send(fd, b, n, flags);
}
static int receiver(void)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	ASSERT(fd >= 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, endpoint);
	ASSERT(!bind(fd, (struct sockaddr *)&a, sizeof(a)));
	return fd;
}
static int output(logger_t *l)
{
	return logger_log_sync_status(l, LOGGER_INFO, "probe", NULL, 0, NULL,
				      "probe");
}
int main(int argc, char **argv)
{
	ASSERT(argc == 2);
	snprintf(endpoint, sizeof(endpoint), "/tmp/lg-sys-old-%ld",
		 (long)getpid());
	unlink(endpoint);
	int fd = receiver();
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_SYSLOG;
	c.async_mode = 0;
	char tag[300];
	memset(tag, 'x', sizeof(tag) - 1);
	tag[sizeof(tag) - 1] = 0;
	if (!strcmp(argv[1], "tag"))
		c.ident = tag;
	logger_t *l = logger_create(&c);
	if (!strcmp(argv[1], "tag")) {
		ASSERT(!l && errno == ENAMETOOLONG);
		close(fd);
		unlink(endpoint);
		return 0;
	}
	ASSERT(l);
	if (!strcmp(argv[1], "flags"))
		ASSERT(fcntl(client, F_GETFL) & O_NONBLOCK);
	else if (!strcmp(argv[1], "pressure")) {
		unsigned i;
		int rc = 0;
		for (i = 0; i < 10000; ++i) {
			rc = output(l);
			if (rc)
				break;
		}
		ASSERT(i < 10000 && rc == -EAGAIN);
	} else if (!strcmp(argv[1], "eintr")) {
		interrupt = 1;
		int rc = output(l);
		ASSERT(rc == -EINTR && sends == 4);
	} else if (!strcmp(argv[1], "restart")) {
		ASSERT(!output(l));
		char b[10000];
		ASSERT(recv(fd, b, sizeof(b), MSG_DONTWAIT) > 0);
		ASSERT(!close(fd));
		ASSERT(!unlink(endpoint));
		ASSERT(output(l) < 0);
		fd = receiver();
		struct timespec ts = { 1, 100000000 };
		while (nanosleep(&ts, &ts))
			ASSERT(errno == EINTR);
		ASSERT(!output(l));
		ASSERT(recv(fd, b, sizeof(b), MSG_DONTWAIT) > 0);
	} else
		ASSERT(!"unknown");
	logger_destroy(l);
	close(fd);
	unlink(endpoint);
	puts("legacy-ABI Syslog probe passed");
	return 0;
}
