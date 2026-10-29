#ifndef LOGGER_FD_H
#define LOGGER_FD_H

#include <stdbool.h>
#include "logger_compiler.h"
#include "logger_fork_fd.h"
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

/*
 * 私有单所有者 fd；只接管已通过 logger_fork_* 登记的资源。
 * 取得失败/移交/关闭后均为 -1，不能用零初始化。
 * 不得复制拥有者或改写 value 绕过移交；C 不提供线性类型保证。
 * 借用不 dup、不固定生命周期；只在拥有者仍存活且未移交时有效。
 * 所有操作要求调用方持有独占访问；不处理取消、longjmp 或并发回收。
 * close 与 fdopendir 必须沿用 fork 资源位图/协调租约，不能绕过为 libc 调用。
 */
struct logger_owned_fd {
	int value;
};

struct logger_borrowed_fd {
	int value;
};

/* 只接管已由 fork 协调器登记的 fd；借用视图不得重造第二个所有者。 */
static inline struct logger_owned_fd LOGGER_MUST_CHECK logger_fd_own(int fd)
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

static inline bool logger_fd_valid(const struct logger_owned_fd *owner)
{
	return owner->value >= 0;
}

/* 显式移交给仍使用原始 int 的所有者；调用方必须接住返回值。 */
static inline int LOGGER_MUST_CHECK logger_fd_take(struct logger_owned_fd *owner)
{
	int fd = owner->value;

	owner->value = -1;
	return fd;
}

/*
 * 保持类型的拥有权转移：源先失效，再返回新的唯一拥有者。
 * 只可移入已为空的目的位置；C 仍允许复制和字段裸访问，调用方必须独占。
 */
static inline struct logger_owned_fd LOGGER_MUST_CHECK
logger_fd_move(struct logger_owned_fd *owner)
{
	return logger_fd_own(logger_fd_take(owner));
}

/*
 * 返回 0/-errno；先失效源，再经过 fork 登记表完成单次关闭。
 * Linux close 即使报 EINTR，也不得恢复源或重试，因为整数可能已被复用。
 * fork 资源锁取得/释放的错误同样由调用者显式处理或上层粘滞诊断保留。
 */
static inline int LOGGER_MUST_CHECK
logger_fd_close_status(struct logger_owned_fd *owner)
{
	int fd = logger_fd_take(owner);

	if (fd < 0)
		return 0;
	if (logger_fork_close(fd) < 0)
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

/*
 * 条件移交：失败后拥有者不变；成功时转给带 fork 资源租约的 DIR。
 * 整段目录遍历必须由同线程 logger_fork_closedir 结束，绝不能直接 closedir，
 * 否则会留下资源协调锁和 fd 位图登记。显式处理关闭错误，不用 cleanup 隐藏。
 */
static inline DIR *LOGGER_MUST_CHECK
logger_fd_fdopendir(struct logger_owned_fd *owner)
{
	if (!logger_fd_valid(owner)) {
		errno = EBADF;
		return NULL;
	}
	DIR *directory = logger_fork_fdopendir(owner->value);

	if (directory)
		owner->value = -1;
	return directory;
}

#endif /* LOGGER_FD_H */
