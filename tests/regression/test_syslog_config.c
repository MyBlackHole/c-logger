#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Boundary objects really end at a PROT_NONE page, not just a short struct_size
 * on a generously-sized current object. */
static void *boundary(const void *data, size_t n, size_t *allocated)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	CHECK(n <= page);
	*allocated = page * 2;
	void *p = mmap(NULL, *allocated, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED);
	CHECK(!mprotect((char *)p + page, page, PROT_NONE));
	memcpy((char *)p + page - n, data, n);
	return p;
}
static int connects;
int __real_connect(int, const struct sockaddr *, socklen_t);
int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t size)
{
	(void)fd;
	(void)size;
	const struct sockaddr_un *a = (const struct sockaddr_un *)addr;
	CHECK(a->sun_family == AF_UNIX && !strcmp(a->sun_path, "/dev/log"));
	++connects;
	errno = ENOENT;
	return -1;
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_SYSLOG;
	c.async_mode = 0;

	size_t n = sizeof(c);
	int expected = ENOENT;
	unsigned char bytes[sizeof(c) + 16];
	memset(bytes, 0, sizeof(bytes));

	if (!strcmp(argv[1], "exact")) {
		c.struct_size = sizeof(c);
	} else if (!strcmp(argv[1], "zero-header")) {
		n = 8;
		c.struct_size = 0;
		c.version = 0;
		expected = EPROTONOSUPPORT;
	} else if (!strcmp(argv[1], "short")) {
		n = 8;
		c.struct_size = 8;
		expected = EINVAL;
	} else if (!strcmp(argv[1], "unknown-version")) {
		n = 8;
		c.struct_size = 8;
		c.version = 999;
		expected = EPROTONOSUPPORT;
	} else if (!strcmp(argv[1], "future-zero")) {
		n = sizeof(c) + 16;
		c.struct_size = (uint32_t)n;
	} else if (!strcmp(argv[1], "future-nonzero")) {
		n = sizeof(c) + 16;
		c.struct_size = (uint32_t)n;
		expected = E2BIG;
	} else {
		CHECK(!"unknown config scenario");
	}

	memcpy(bytes, &c, sizeof(c));
	if (!strcmp(argv[1], "future-nonzero"))
		bytes[sizeof(c)] = 1;

	size_t allocated;
	void *base = boundary(bytes, n, &allocated);
	logger_t *l = logger_create(
		(const logger_config_t *)((char *)base + allocated / 2 - n));
	CHECK(!l && errno == expected);
	CHECK(connects == (expected == ENOENT ? 1 : 0));
	CHECK(!munmap(base, allocated));
	printf("uapi config read passed: %s current=%zu\n",
	       argv[1], sizeof(c));
	return 0;
}
