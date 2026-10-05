#define _GNU_SOURCE
#include "audit_internal.h"
#include "support.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef F_OFD_SETLK
#define F_OFD_SETLK 37
#endif

int __wrap_fcntl(int fd, int cmd, ...)
{
	(void)fd;
	if (cmd == F_OFD_SETLK) {
		errno = EINVAL;
		return -1;
	}
	errno = EPROTO;
	return -1;
}

int main(void)
{
	char path[] = "/tmp/audit-ofd-platform-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(audit_writer_lock_fd(fd) == -ENOTSUP);
	CHECK(close(fd) == 0);
	CHECK(unlink(path) == 0);
	return 0;
}
