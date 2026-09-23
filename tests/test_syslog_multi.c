#include "logger.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(void)
{
	const char *p = "./test-dev-log";
	unlink(p);
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0)
		return 1;
	struct sockaddr_un a = { 0 };
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", p);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
		return 2;
	setenv("LOGGER_SYSLOG_PATH", p, 1);
	logger_config_t x = LOGGER_DEFAULT_CONFIG();
	x.outputs = LOGGER_OUT_SYSLOG;
	x.async_mode = 0;
	x.ident = "alpha";
	logger_config_t y = LOGGER_DEFAULT_CONFIG();
	y.outputs = LOGGER_OUT_SYSLOG;
	y.async_mode = 0;
	y.ident = "beta";
	logger_t *A = logger_create(&x), *B = logger_create(&y);
	if (!A || !B)
		return 3;
	logger_log(A, LOGGER_INFO, "m", __FILE__, __LINE__, __func__, "one");
	logger_log(B, LOGGER_ERROR, "m", __FILE__, __LINE__, __func__, "two");
	char b1[8192], b2[8192];
	ssize_t n1 = recv(fd, b1, sizeof(b1) - 1, 0),
		n2 = recv(fd, b2, sizeof(b2) - 1, 0);
	if (n1 < 0 || n2 < 0)
		return 4;
	b1[n1] = 0;
	b2[n2] = 0;
	logger_destroy(A);
	logger_destroy(B);
	close(fd);
	unlink(p);
	if (!(strstr(b1, "alpha") || strstr(b2, "alpha")))
		return 5;
	if (!(strstr(b1, "beta") || strstr(b2, "beta")))
		return 6;
	return 0;
}
