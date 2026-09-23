#define _GNU_SOURCE
#include "logger_file.h"
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
int logger_file_rename_noreplace(int dirfd, const char *from, const char *to)
{
#ifdef SYS_renameat2
	int rc = (int)syscall(SYS_renameat2, dirfd, from, dirfd, to,
			      RENAME_NOREPLACE);
	if (rc && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP))
		errno = ENOTSUP;
	return rc;
#else
	(void)dirfd;
	(void)from;
	(void)to;
	errno = ENOTSUP;
	return -1;
#endif
}
