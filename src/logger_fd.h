#ifndef LOGGER_FD_H
#define LOGGER_FD_H

#include "logger_compiler.h"
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

/* 私有单所有者 fd；取得失败/移交/关闭后均为 -1，不能用零初始化。
 * 不得复制拥有者或改写 value 绕过移交；C 不提供线性类型保证。
 * 借用不 dup、不固定生命周期；只在拥有者仍存活且未移交时有效。
 * 所有操作要求调用方持有独占访问；不处理取消、longjmp 或并发回收。
 */
struct logger_owned_fd {
	int value;
};

struct logger_borrowed_fd {
	int value;
};

/* 只接管刚取得或显式移交的原始 fd；不能用借用值制造第二个所有者。 */
static inline LOGGER_MUST_CHECK struct logger_owned_fd logger_fd_own(int fd)
{
	return (struct logger_owned_fd){ .value = fd };
}

static inline struct logger_borrowed_fd
logger_fd_borrow(const struct logger_owned_fd *owner)
{
	return (struct logger_borrowed_fd){ .value = owner->value };
}

/* POSIX 边界的只读借用；返回的整数不携带关闭/移交权。 */
static inline int logger_fd_number(struct logger_borrowed_fd borrowed)
{
	return borrowed.value;
}

static inline int logger_fd_valid(const struct logger_owned_fd *owner)
{
	return owner->value >= 0;
}

/* 显式移交给仍使用原始 int 的所有者；调用方必须接住返回值。 */
static inline LOGGER_MUST_CHECK int logger_fd_take(struct logger_owned_fd *owner)
{
	int fd = owner->value;

	owner->value = -1;
	return fd;
}

/* 返回 0 或 -errno；必须在 close 前失效源。Linux close 即使报 EINTR，
 * 也不得恢复源或重试，因为该整数可能已属于另一个资源。
 */
static inline LOGGER_MUST_CHECK int
logger_fd_close_status(struct logger_owned_fd *owner)
{
	int fd = logger_fd_take(owner);

	if (fd < 0)
		return 0;
	if (close(fd) < 0)
		return -errno;
	return 0;
}

/* 仅用于已有主错误或无错误回执的词法回滚；有业务关闭错误必须显式处理。 */
static inline void logger_fd_cleanup(struct logger_owned_fd *owner)
{
	int saved = errno;
	int rc LOGGER_MAYBE_UNUSED = logger_fd_close_status(owner);

	errno = saved;
}

/* 条件移交：失败后拥有者不变；只有成功才交给 DIR，最终由 closedir 释放。
 * 调用者仍须显式处理 closedir 的错误，不能将其隐藏在无回执的 cleanup 中。
 */
static inline LOGGER_MUST_CHECK DIR *
logger_fd_fdopendir(struct logger_owned_fd *owner)
{
	if (!logger_fd_valid(owner)) {
		errno = EBADF;
		return NULL;
	}
	DIR *directory = fdopendir(owner->value);

	if (directory)
		owner->value = -1;
	return directory;
}

#endif
