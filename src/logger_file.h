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
/* Private movable owner. No pointers reference storage inside this struct.
 * All operations are serialized by the containing logger's emit_mu.
 * Regular files are coordinated by <active>.logger.lock, never unlinked here.
 */
typedef struct {
	char path[LOGGER_PATH_MAX]; /* diagnostic only; never re-resolved after bind */
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
/* Reserve before Audit recovery; no active-file creation or modification.
 * A reserved owner is consumed only by successful open/move into a logger. */
int logger_file_reserve(logger_file_t *, const char *, logger_rotation_config_t,
			mode_t);
int logger_file_open_reserved(logger_file_t *);
int logger_file_close_status(logger_file_t *); /* 0 / -errno; releases all fds */
void logger_file_close(logger_file_t *);
int logger_file_sync(logger_file_t *); /* 0 / -errno, caller holds emit_mu */
int logger_file_reopen(logger_file_t *); /* 0 / -1 + errno */
int logger_file_write(logger_file_t *, const char *, size_t,
		      const struct timespec *, int);
int logger_file_writev(logger_file_t *, struct iovec *, int, size_t,
		       const struct timespec *, int);
int logger_file_offset_get(logger_file_t *, uint64_t *);
/* Separate translation-unit helper permits link-time fault tests. Linux
 * syscall, no overwriting rename fallback; unsupported -> ENOTSUP. */
int logger_file_rename_noreplace(int, const char *, const char *);
#endif
