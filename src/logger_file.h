#ifndef LOGGER_FILE_H
#define LOGGER_FILE_H
#include "logger.h"
#include "logger_fd.h"
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#define LOGGER_PATH_MAX 4096
#define LOGGER_FILENAME_MAX 255
#define LOGGER_ARCHIVE_ATTEMPTS 1024u
/* 私有、可移动的所有者。结构体内部没有指针指向自身存储，因此整体移动安全。
 * 所有操作由所属 logger 的 emit_mu 串行化。
 * 普通文件通过 <active>.logger.lock 做跨实例协调，这里不会删除该锁文件。
 */
struct logger_file {
	char path[LOGGER_PATH_MAX]; /* 仅用于诊断；绑定后绝不重新解析路径 */
	char name[LOGGER_FILENAME_MAX + 1];
	char lock_name[LOGGER_FILENAME_MAX + 1];
	char stem[LOGGER_FILENAME_MAX + 1];
	char ext[LOGGER_FILENAME_MAX + 1];
	logger_rotation_config_t rotation;
	mode_t mode;
	struct logger_owned_fd fd;
	struct logger_owned_fd dir_fd;
	struct logger_owned_fd lock_fd;
	int managed;
	int detached;
	int switch_error;
	size_t current_size;
	int current_day;
	int dir_dirty;
	dev_t device;
	ino_t inode;
	struct timespec archive_time;
	int archive_time_valid;
	int iov_max;
	logger_file_metrics_t metrics;
};
#define LOGGER_FILE_EMPTY      \
	((struct logger_file){ \
		.fd = { -1 }, .dir_fd = { -1 }, .lock_fd = { -1 } })
int logger_file_init(struct logger_file *f, const char *path,
		     logger_rotation_config_t rotation, mode_t mode);
int logger_file_close_status(
	struct logger_file *f); /* 0 / -errno；释放全部 fd */
void logger_file_close(struct logger_file *f);
int logger_file_sync(
	struct logger_file *f); /* 0 / -errno；调用者必须持有 emit_mu */
int logger_file_reopen(struct logger_file *f); /* 0 / -1 + errno */
int logger_file_write(struct logger_file *f, const char *p, size_t n,
		      const struct timespec *t, int sync);
int logger_file_writev(struct logger_file *f, struct iovec *v, int count,
		       size_t n, const struct timespec *t, int sync);
/* 独立编译单元便于链接时故障测试。使用 Linux 系统调用，
 * 不提供会覆盖目标的重命名回退；平台不支持时返回 ENOTSUP。
 */
int logger_file_rename_noreplace(int dirfd, const char *from, const char *to);
#endif
