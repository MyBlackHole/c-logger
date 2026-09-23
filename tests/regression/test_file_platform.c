#define _GNU_SOURCE
#include "support.h"
#include "logger_file.h"
#include <stdarg.h>
#include <fcntl.h>
#include <sys/syscall.h>
static int injected;
long __real_syscall(long, ...);
long __wrap_syscall(long number, ...)
{
#ifdef SYS_renameat2
	CHECK(number == SYS_renameat2);
	va_list ap;
	va_start(ap, number);
	int a = va_arg(ap, int);
	const char *from = va_arg(ap, const char *);
	int b = va_arg(ap, int);
	const char *to = va_arg(ap, const char *);
	unsigned flags = va_arg(ap, unsigned);
	va_end(ap);
	CHECK(a == b && flags == 1u);
	if (injected) {
		errno = injected;
		return -1;
	}
	return __real_syscall(number, a, from, b, to, flags);
#else
	(void)number;
	CHECK(0);
	return -1;
#endif
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	int fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	int a = openat(fd, "source", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(a >= 0);
	CHECK(write(a, "source-data", 11) == 11 && close(a) == 0);
	int b = openat(fd, "destination", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(b >= 0);
	CHECK(write(b, "existing", 8) == 8 && close(b) == 0);
	if (!strcmp(argv[1], "enosys"))
		injected = ENOSYS;
	else if (!strcmp(argv[1], "einval"))
		injected = EINVAL;
	else if (!strcmp(argv[1], "eopnotsupp"))
		injected = EOPNOTSUPP;
	else
		CHECK(!strcmp(argv[1], "real"));
	CHECK(logger_file_rename_noreplace(fd, "source", "destination") == -1);
	CHECK(errno == (injected ? ENOTSUP : EEXIST));
	CHECK(file_size("source") == 11 && file_size("destination") == 8);
	CHECK(unlink("destination") == 0);
	injected = 0;
	CHECK(logger_file_rename_noreplace(fd, "source", "destination") == 0);
	CHECK(access("source", F_OK) != 0 && file_size("destination") == 11);
	CHECK(unlink("destination") == 0 && close(fd) == 0);
	return 0;
}
