#ifndef LOGGER_FORK_FD_H
#define LOGGER_FORK_FD_H

#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>

/*
 * 私有描述符所有者入口，返回值遵循对应 libc 调用。
 * 所有调用者必须保持原有的取消/工作线程寿命约束；不允许信号或回调重入。
 * 取得与登记、移交与关闭都在同一个 fork 协调区间完成。
 * 登记表只保存 fd 值，不引用实例、候选栈变量或 DIR 对象。
 */
int logger_fork_open(const char *path, int flags, mode_t mode);
int logger_fork_openat(int dir, const char *path, int flags, mode_t mode);
int logger_fork_socket(int domain, int type, int protocol);
int logger_fork_close(int fd);
DIR *logger_fork_fdopendir(int fd);
int logger_fork_closedir(DIR *dir);

/* 由唯一进程注册入口调用；child 只关闭继承引用，不解锁旧 mutex。 */
void logger_fork_fd_prepare(void);
void logger_fork_fd_parent(void);
void logger_fork_fd_child(void);
/* 保留子方快照/关闭错误供下一阶段初始化判定，不清除既有进程拒绝。 */
int logger_fork_fd_child_error(void);
/* 同线程处于 prepare/parent 协调区间时，其他 handler 不得重入普通 API。 */
bool logger_fork_fd_preparing(void);
int logger_fork_resource_enter(void);
int logger_fork_resource_leave(int error);
void logger_fork_resource_poison(int error);
int logger_fork_fd_child_reinit(void);
bool logger_fork_fd_child_valid(void);
#endif /* LOGGER_FORK_FD_H */
