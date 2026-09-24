#ifndef LOGGER_FILE_H
#define LOGGER_FILE_H
#include "logger.h"
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#define LOGGER_PATH_MAX 4096
#define LOGGER_FILENAME_MAX 255
#define LOGGER_ARCHIVE_ATTEMPTS 1024u
/* 私有、可 move 的 owner。结构体内部没有指针指向自身存储，因此整体 move 安全。
 * 所有操作由所属 logger 的 emit_mu 串行化。
 * 普通文件通过 <active>.logger.lock 做跨实例协调，这里不会 unlink 该 lock 文件。
 */
typedef struct {
	char path[LOGGER_PATH_MAX]; /* 仅用于诊断；bind 后绝不重新解析路径 */
	char name[LOGGER_FILENAME_MAX + 1];
	char lock_name[LOGGER_FILENAME_MAX + 1];
	char stem[LOGGER_FILENAME_MAX + 1], ext[LOGGER_FILENAME_MAX + 1];
	logger_rotation_config_t rotation;
	mode_t mode;
	int fd, dir_fd, lock_fd;
	int managed, detached, switch_error;
	size_t current_size;
	int current_day, dir_dirty;
	dev_t device;
	ino_t inode;
	struct timespec archive_time;
	int archive_time_valid, iov_max;
} logger_file_t;
#define LOGGER_FILE_EMPTY \
	((logger_file_t){ .fd = -1, .dir_fd = -1, .lock_fd = -1 })
int logger_file_init(logger_file_t *, const char *, logger_rotation_config_t,
		     mode_t);
/* Audit recovery 前先 reserve；此阶段不创建或修改 active file。
 * reserved owner 只有在成功 open/move 进 logger 后才被消费。 */
int logger_file_reserve(logger_file_t *, const char *, logger_rotation_config_t,
			mode_t);
int logger_file_open_reserved(logger_file_t *);
int logger_file_close_status(logger_file_t *); /* 0 / -errno；释放全部 fd */
void logger_file_close(logger_file_t *);
int logger_file_sync(logger_file_t *); /* 0 / -errno；调用者必须持有 emit_mu */
int logger_file_reopen(logger_file_t *); /* 0 / -1 + errno */
int logger_file_write(logger_file_t *, const char *, size_t,
		      const struct timespec *, int);
int logger_file_writev(logger_file_t *, struct iovec *, int, size_t,
		       const struct timespec *, int);
int logger_file_offset_get(logger_file_t *, uint64_t *);
/* 独立 translation unit 便于 link-time fault test。使用 Linux syscall，
 * 不提供会覆盖目标的 rename fallback；平台不支持时返回 ENOTSUP。 */
int logger_file_rename_noreplace(int, const char *, const char *);
#endif
