#define _GNU_SOURCE
#include "logger_file.h"
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#ifdef SYS_renameat2
int logger_file_rename_noreplace(int dirfd, const char *from, const char *to)
{
	int rc = (int)syscall(SYS_renameat2, dirfd, from, dirfd, to,
			      RENAME_NOREPLACE);
	if (rc && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP))
		errno = ENOTSUP;
	return rc;
}
#else
int logger_file_rename_noreplace(int dirfd, const char *from, const char *to)
{
	(void)dirfd;
	(void)from;
	(void)to;
	errno = ENOTSUP;
	return -1;
}
#endif /* SYS_renameat2 */
