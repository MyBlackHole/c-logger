#define _GNU_SOURCE
#include "logger_v1.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
/* Build with frozen OLD header, link to NEW library. Actual Syslog I/O; no new
 * config field or API. Redirect /dev/log in this executable only. */
static char endpoint[108];
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t n)
{
	const struct sockaddr_un *old = (const struct sockaddr_un *)addr;
	if (old->sun_family != AF_UNIX || strcmp(old->sun_path, "/dev/log"))
		return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, endpoint);
	(void)n;
	return __real_connect(fd, (struct sockaddr *)&a, sizeof(a));
}
int main(void)
{
	snprintf(endpoint, sizeof(endpoint), "/tmp/lg-v1-%ld", (long)getpid());
	unlink(endpoint);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, endpoint);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)))
		return 2;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_SYSLOG;
	cfg.async_mode = 0;
	cfg.ident = "old-client";
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	char *base = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE))
		return 3;
	void *oldcfg = base + page - sizeof(cfg);
	memcpy(oldcfg, &cfg, sizeof(cfg));
	logger_t *l = logger_create(oldcfg);
	if (!l) {
		perror("old config create");
		return 4;
	}
	if (logger_log_sync_status(l, LOGGER_INFO, "old", NULL, 0, NULL,
				   "old-consumer"))
		return 5;
	char b[8192];
	ssize_t count = recv(fd, b, sizeof(b) - 1, MSG_DONTWAIT);
	if (count <= 0)
		return 6;
	b[count] = 0;
	if (strncmp(b, "<14>old-client[", 14) || !strstr(b, "old-consumer"))
		return 7;
	if (logger_flush_instance_status(l) || logger_destroy_status(l))
		return 8;
	if (munmap(base, page * 2) || close(fd) || unlink(endpoint))
		return 9;
	printf("old independent consumer passed; old struct_size=%zu\n",
	       sizeof(cfg));
	return 0;
}
