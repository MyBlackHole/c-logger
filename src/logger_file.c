#define _GNU_SOURCE
#include "logger_libc.h"
#include "logger_file.h"
#include "logger_fault.h"
#include "logger_fd.h"
#include "logger_fork_fd.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int internal_rotation(const logger_file_t *f)
{
	return f->rotation.mode == LOGGER_ROTATE_SIZE ||
	       f->rotation.mode == LOGGER_ROTATE_DAILY ||
	       f->rotation.mode == LOGGER_ROTATE_SIZE_DAILY;
}

static int wall_day(time_t sec, int *day)
{
	struct tm t;
	if (!logger_libc_localtime_r(&sec, &t) || t.tm_year < -1900 ||
	    t.tm_year > 8099)
		return -EOVERFLOW;
	*day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
	return 0;
}

static int sync_data(logger_file_t *f, int fd)
{
	++f->metrics.data_sync_attempts;
	int rc;
	do {
		rc = logger_fault_should_fail(LOGGER_FAULT_FILE_FSYNC) ?
			     -1 :
			     fsync(fd);
	} while (rc < 0 && errno == EINTR);
	if (rc < 0) {
		int error = errno ? errno : EIO;
		++f->metrics.data_sync_failures;
		f->metrics.last_sync_error = error;
		return -error;
	}
	return 0;
}

static int sync_directory(logger_file_t *f)
{
	++f->metrics.directory_sync_attempts;
	int rc;
	do {
		rc = fsync(f->dir_fd);
	} while (rc < 0 && errno == EINTR);
	if (rc < 0) {
		int error = errno ? errno : EIO;
		++f->metrics.directory_sync_failures;
		f->metrics.last_sync_error = error;
		return -error;
	}
	f->dir_dirty = 0;
	return 0;
}

int logger_file_close_status(logger_file_t *f)
{
	if (!f)
		return -EINVAL;
	int error = 0;
	int *fds[] = { &f->fd, &f->lock_fd, &f->dir_fd };
	/* 保持所有权直到数据 fd 关闭。不显式 LOCK_UN：fork/dup 引用共享 flock；
	 * 只关闭此所有者的引用。
	 */
	for (unsigned i = 0; i < sizeof(fds) / sizeof(*fds); ++i) {
		/* 字段先失效，再由唯一栈上拥有者消费 fork 位图登记。
		 * 一处 close 失败不能中断其余资源的最终释放。
		 */
		struct logger_owned_fd retiring = logger_fd_own(*fds[i]);

		*fds[i] = -1;
		int rc = logger_fd_close_status(&retiring);

		if (rc && !error)
			error = -rc;
	}
	return error ? -error : 0;
}

void logger_file_close(logger_file_t *f)
{
	int saved = errno;
	(void)logger_file_close_status(f);
	errno = saved;
}

static int same_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int regular_single_link(const struct stat *st)
{
	if (!S_ISREG(st->st_mode))
		return -EINVAL;
	if (st->st_nlink != 1)
		return -EMLINK;
	if (st->st_size < 0 || (uintmax_t)st->st_size > SIZE_MAX)
		return -EOVERFLOW;
	return 0;
}

static int reserved_basename(const char *name, size_t length)
{
	static const char suffix[] = ".logger.lock";
	size_t n = sizeof(suffix) - 1u;
	return length >= n && !memcmp(name + length - n, suffix, n);
}

static int check_named_fd(int dirfd, const char *name, int fd, struct stat *st)
{
	struct stat named;
	if (fstat(fd, st) || fstatat(dirfd, name, &named, AT_SYMLINK_NOFOLLOW))
		return -errno;
	if (S_ISLNK(named.st_mode))
		return -ELOOP;
	return same_file(st, &named) ? 0 : -ESTALE;
}

static int check_owner(logger_file_t *f)
{
	if (!f->managed)
		return 0;
	struct stat st;
	int rc = check_named_fd(f->dir_fd, f->lock_name, f->lock_fd, &st);
	return rc ? rc : regular_single_link(&st);
}

static int bind_path(logger_file_t *f, const char *path)
{
	if (!path || !*path)
		return -EINVAL;
	size_t length = strnlen(path, LOGGER_PATH_MAX);
	if (length == LOGGER_PATH_MAX)
		return -ENAMETOOLONG;
	memcpy(f->path, path, length + 1);
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	size_t n = strlen(base);
	if (!n || !strcmp(base, ".") || !strcmp(base, ".."))
		return -EINVAL;
	if (n > LOGGER_FILENAME_MAX)
		return -ENAMETOOLONG;
	if (reserved_basename(base, n))
		return -EINVAL;
	memcpy(f->name, base, n + 1);
	char dir[LOGGER_PATH_MAX];
	if (slash) {
		size_t dlen = slash == path ? 1u : (size_t)(slash - path);
		memcpy(dir, path, dlen);
		dir[dlen] = 0;
	} else
		memcpy(dir, ".", 2);
	f->dir_fd =
		logger_fork_open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (f->dir_fd < 0)
		return -errno;
	errno = 0;
	long maximum = fpathconf(f->dir_fd, _PC_NAME_MAX);
	if (maximum < 0 && errno)
		return -errno;
	if (maximum < 0 || maximum > LOGGER_FILENAME_MAX)
		maximum = LOGGER_FILENAME_MAX;
	if (n > (size_t)maximum)
		return -ENAMETOOLONG;

	struct stat st;
	if (fstatat(f->dir_fd, f->name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (S_ISLNK(st.st_mode))
			return -ELOOP;
		if (S_ISCHR(st.st_mode) &&
		    f->rotation.mode == LOGGER_ROTATE_NONE) {
			f->managed =
				0; /* /dev/null 和 /dev/full 用于连通测试，不用于持久性验证 */
			return 0;
		}
		int rc = regular_single_link(&st);
		if (rc)
			return rc;
	} else if (errno != ENOENT)
		return -errno;
	f->managed = 1;
	const char suffix[] = ".logger.lock";
	if (n + sizeof(suffix) - 1 > (size_t)maximum)
		return -ENAMETOOLONG;
	if (internal_rotation(f) && n + 24u > (size_t)maximum)
		return -ENAMETOOLONG;
	memcpy(f->lock_name, f->name, n);
	memcpy(f->lock_name + n, suffix, sizeof(suffix));
	const char *dot = strrchr(base, '.');
	if (dot && dot != base) {
		size_t stem = (size_t)(dot - base);
		memcpy(f->stem, base, stem);
		f->stem[stem] = 0;
		memcpy(f->ext, dot, strlen(dot) + 1);
	} else
		memcpy(f->stem, base, n + 1);
	return 0;
}

/* 在创建或打开活动文件前预留 Logger 文件所有权。 */
static int logger_file_reserve(logger_file_t *f, const char *path,
			       logger_rotation_config_t rotation, mode_t mode)
{
	if (!f) {
		errno = EINVAL;
		return -1;
	}
	*f = LOGGER_FILE_EMPTY;
	if ((unsigned)rotation.mode > LOGGER_ROTATE_EXTERNAL ||
	    (mode & ~0777u)) {
		errno = EINVAL;
		return -1;
	}
	f->rotation = rotation;
	f->mode = mode ? mode : 0640;
	long iov_max = sysconf(_SC_IOV_MAX);
	f->iov_max = iov_max > 0 && iov_max <= INT_MAX ? (int)iov_max : 16;
	int rc = bind_path(f, path);
	if (!rc && f->managed) {
		f->lock_fd = logger_fork_openat(f->dir_fd, f->lock_name,
						O_RDWR | O_CREAT | O_CLOEXEC |
							O_NOFOLLOW | O_NONBLOCK,
						0600);
		if (f->lock_fd < 0)
			rc = -errno;
		if (!rc)
			rc = check_owner(f);
		if (!rc) {
			int locked;
			do {
				locked = flock(f->lock_fd, LOCK_EX | LOCK_NB);
			} while (locked && errno == EINTR);
			if (locked)
				rc = errno == EAGAIN || errno == EWOULDBLOCK ?
					     -EBUSY :
					     -errno;
		}
		if (!rc)
			rc = check_owner(f);
	}
	if (rc) {
		logger_file_close(f);
		errno = -rc;
		return -1;
	}
	return 0;
}

/* 不截断任何文件。O_NONBLOCK 使被替换的 FIFO 在验证时失败，
 * 而不是让 logger_fork_open() 挂起。目录祖先必须可信。
 */
static int open_candidate(logger_file_t *f, int exclusive, int *out,
			  struct stat *st)
{
	int rc = check_owner(f);
	if (rc)
		return rc;
	int flags = O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
	if (f->managed)
		flags |= O_CREAT;
	if (exclusive)
		flags |= O_EXCL;
	int candidate = logger_fork_openat(f->dir_fd, f->name, flags, f->mode);
	struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(candidate);
	if (!logger_fd_valid(&fd))
		return -errno;
	rc = check_named_fd(f->dir_fd, f->name,
			    logger_fd_number(logger_fd_borrow(&fd)), st);
	if (!rc)
		rc = f->managed ? regular_single_link(st) :
				  (S_ISCHR(st->st_mode) ? 0 : -EINVAL);
	if (rc)
		return rc;

	/* 所有权转移：词法作用域候选对象 -> 调用者。调用者要么
	 * 将其装入 logger_file_t，要么在失败时显式关闭。
	 */
	*out = logger_fd_take(&fd);
	/* 之前不存在的活动文件可能已被创建。即使它原本存在，也保守地
	 * 同步绑定目录；这绝不依赖当前工作目录。
	 */
	if (f->managed)
		f->dir_dirty = 1;
	return 0;
}

static void install_fd(logger_file_t *f, int fd, const struct stat *st, int day)
{
	f->fd = fd;
	f->device = st->st_dev;
	f->inode = st->st_ino;
	f->current_size = f->managed ? (size_t)st->st_size : 0;
	f->current_day = day;
	f->detached = 0;
	f->switch_error = 0;
}

static int logger_file_open_reserved(logger_file_t *f)
{
	if (!f || f->dir_fd < 0 || f->fd >= 0) {
		errno = EINVAL;
		return -1;
	}
	int day = 0, fd = -1;
	int rc = wall_day(time(NULL), &day);
	struct stat st;
	if (!rc)
		rc = open_candidate(f, 0, &fd, &st);
	if (rc) {
		errno = -rc;
		return -1;
	}
	install_fd(f, fd, &st, day);
	return 0;
}

int logger_file_init(logger_file_t *f, const char *path,
		     logger_rotation_config_t rotation, mode_t mode)
{
	if (logger_file_reserve(f, path, rotation, mode))
		return -1;
	if (!logger_file_open_reserved(f))
		return 0;
	int error = errno;
	logger_file_close(f);
	errno = error;
	return -1;
}

int logger_file_sync(logger_file_t *f)
{
	if (!f || f->fd < 0)
		return -EBADF;
	int rc = sync_data(f, f->fd);
	if (!rc && f->dir_dirty)
		rc = sync_directory(f);
	if (!rc && f->detached)
		rc = -(f->switch_error ? f->switch_error : EIO);
	f->metrics.last_error = rc < 0 ? -rc : 0;
	return rc;
}

static int file_reopen_impl(logger_file_t *f)
{
	int day = 0, fd = -1;
	struct stat st;
	int rc = wall_day(time(NULL), &day);
	if (!rc)
		rc = open_candidate(f, 0, &fd, &st);
	/* 旧 fd 同步期间，新候选仍由词法拥有者负责清理。
	 * 只有全部准备成功，才将它移交给长期 f->fd。
	 */
	struct logger_owned_fd candidate LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(fd);
	if (!rc && f->managed)
		rc = sync_data(f, f->fd);
	if (!rc && f->dir_dirty)
		rc = sync_directory(f);
	if (rc) {
		errno = -rc;
		return -1;
	}
	int old_fd = f->fd;
	install_fd(f, logger_fd_take(&candidate), &st, day);
	if (logger_fork_close(old_fd) < 0)
		return -1; /* 新 fd 已装入；不要重试 close */
	return 0;
}

int logger_file_reopen(logger_file_t *f)
{
	if (!f || f->fd < 0) {
		errno = EBADF;
		return -1;
	}
	++f->metrics.reopen_attempts;
	int rc = file_reopen_impl(f);
	if (rc) {
		int error = errno ? errno : EIO;
		++f->metrics.reopen_failures;
		f->metrics.last_reopen_error = error;
		f->metrics.last_error = error;
	} else {
		++f->metrics.reopen_successes;
		f->metrics.last_error = 0;
	}
	return rc;
}

static int check_active(logger_file_t *f)
{
	struct stat st;
	int rc = check_owner(f);
	if (!rc)
		rc = check_named_fd(f->dir_fd, f->name, f->fd, &st);
	if (!rc && (st.st_dev != f->device || st.st_ino != f->inode))
		rc = -ESTALE;
	if (!rc)
		rc = regular_single_link(&st);
	return rc;
}

static int timestamp_valid(struct timespec t)
{
	struct tm tm;
	return t.tv_nsec >= 0 && t.tv_nsec < 1000000000 &&
	       logger_libc_gmtime_r(&t.tv_sec, &tm) && tm.tm_year >= -1900 &&
	       tm.tm_year <= 8099;
}

static int advance_microsecond(struct timespec *t)
{
	if (t->tv_nsec < 999999000L)
		t->tv_nsec += 1000;
	else {
		/* 不要在 32 位目标上使 time_t 溢出。 */
		if ((intmax_t)t->tv_sec == INTMAX_MAX ||
		    (sizeof(time_t) == 4 && t->tv_sec == (time_t)INT32_MAX))
			return -EOVERFLOW;
		++t->tv_sec;
		t->tv_nsec = 0;
	}
	return timestamp_valid(*t) ? 0 : -EOVERFLOW;
}

static int archive_name(logger_file_t *f, const struct timespec *t, char *out,
			size_t cap)
{
	struct tm tm;
	char stamp[32];
	if (!timestamp_valid(*t) || !logger_libc_gmtime_r(&t->tv_sec, &tm) ||
	    logger_libc_strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &tm) !=
		    15u)
		return -EOVERFLOW;
	int n = logger_libc_snprintf(out, cap, "%s.%s.%06ldZ%s", f->stem, stamp,
				     t->tv_nsec / 1000, f->ext);
	return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static int archive_matches(logger_file_t *f, const char *name)
{
	size_t stem = strlen(f->stem), ext = strlen(f->ext),
	       length = strlen(name);
	if (length != stem + 24u + ext || memcmp(name, f->stem, stem) ||
	    name[stem] != '.')
		return 0;
	const char *p = name + stem + 1;
	for (size_t i = 0; i < 23; ++i) {
		if (i == 8) {
			if (p[i] != 'T')
				return 0;
		} else if (i == 15) {
			if (p[i] != '.')
				return 0;
		} else if (i == 22) {
			if (p[i] != 'Z')
				return 0;
		} else if (p[i] < '0' || p[i] > '9')
			return 0;
	}
	return !memcmp(p + 23, f->ext, ext);
}

static int retention(logger_file_t *f, time_t now)
{
	if (!f->rotation.retention_days)
		return 0;
	/* 独立的目录描述避免与绑定目录共享 readdir 的偏移。 */
	struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(logger_fork_openat(
			f->dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0));
	if (!logger_fd_valid(&fd))
		return -errno;
	DIR *d = logger_fd_fdopendir(&fd);
	if (!d)
		return -errno;
	/* fork DIR 租约已持有资源锁且源已失效；必须由同线程
	 * logger_fork_closedir() 消费租约，不能直接调用 libc closedir。
	 */
	int rc = 0, changed = 0;
	uintmax_t seconds = (uintmax_t)f->rotation.retention_days * 86400u;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(d);
		if (!entry) {
			if (errno)
				rc = -errno;
			break;
		}
		if (!archive_matches(f, entry->d_name))
			continue;
		struct stat st;
		if (fstatat(f->dir_fd, entry->d_name, &st,
			    AT_SYMLINK_NOFOLLOW)) {
			rc = -errno;
			break;
		}
		if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
		    (st.st_dev == f->device && st.st_ino == f->inode))
			continue;
		/* difftime 避免有符号 time_t 减法溢出。 */
		if (difftime(now, st.st_mtime) <= (double)seconds)
			continue;
		if (unlinkat(f->dir_fd, entry->d_name, 0)) {
			rc = -errno;
			break;
		}
		++f->metrics.retention_deletions;
		changed = 1;
	}
	if (logger_fork_closedir(d) && !rc)
		rc = -errno;
	if (changed) {
		f->dir_dirty = 1;
		int synced = sync_directory(f);
		if (!rc)
			rc = synced;
	}
	return rc;
}

static int rotate_impl(logger_file_t *f)
{
	int rc = check_active(f);
	if (rc)
		return rc;
	struct timespec t;
	if (clock_gettime(CLOCK_REALTIME, &t))
		return -errno;
	if (!timestamp_valid(t))
		return -EOVERFLOW;
	t.tv_nsec -= t.tv_nsec % 1000;
	if (f->archive_time_valid && (t.tv_sec < f->archive_time.tv_sec ||
				      (t.tv_sec == f->archive_time.tv_sec &&
				       t.tv_nsec <= f->archive_time.tv_nsec))) {
		t = f->archive_time;
		rc = advance_microsecond(&t);
		if (rc)
			return rc;
	}
	rc = logger_file_sync(f);
	if (rc)
		return rc;
	char name[LOGGER_FILENAME_MAX + 1];
	unsigned attempt;
	for (attempt = 0; attempt < LOGGER_ARCHIVE_ATTEMPTS; ++attempt) {
		rc = archive_name(f, &t, name, sizeof(name));
		if (rc)
			return rc;
		if (!logger_fault_should_fail(LOGGER_FAULT_FILE_RENAME) &&
		    !logger_file_rename_noreplace(f->dir_fd, f->name, name))
			break;
		int error = errno;
		if (error != EEXIST)
			return -error;
		rc = advance_microsecond(&t);
		if (rc)
			return rc;
	}
	if (attempt == LOGGER_ARCHIVE_ATTEMPTS)
		return -EEXIST;
	/* 旧 fd 现在属于归档。即使打开或同步新活动文件失败，也绝不向其追加。
	 * 只有显式重新打开才可能恢复。
	 */
	f->detached = 1;
	f->switch_error = EIO;
	f->dir_dirty = 1;
	f->archive_time = t;
	f->archive_time_valid = 1;
	logger_fault_crash_if_requested("file_after_archive_rename");
	rc = sync_directory(f);
	if (!rc)
		logger_fault_crash_if_requested("file_after_archive_dirsync");
	int candidate_fd = -1, day = 0;
	struct stat st;
	if (!rc)
		rc = wall_day(t.tv_sec, &day);
	if (!rc)
		rc = open_candidate(f, 1, &candidate_fd, &st);
	/* 已归档的旧 fd 不属于候选；新活动 fd 在安装前独占持有。
	 * 崩溃切点与数据/目录持久化顺序保持不变。
	 */
	struct logger_owned_fd candidate LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(candidate_fd);
	if (!rc)
		logger_fault_crash_if_requested("file_after_active_open");
	if (!rc) {
		int new_fd = logger_fd_number(logger_fd_borrow(&candidate));
		rc = sync_data(f, new_fd);
	}
	/* 先持久化新的空 inode，再同步包含新目录项的父目录。 */
	if (!rc)
		rc = sync_directory(f);
	if (!rc)
		logger_fault_crash_if_requested("file_after_active_dirsync");
	if (rc) {
		/* 与旧路径相同：先回收新候选，随后记录切换错误；
		 * close(EINTR/EIO) 不能覆盖失败的 fsync/目录同步主错误。
		 */
		int close_rc LOGGER_MAYBE_UNUSED =
			logger_fd_close_status(&candidate);
		f->switch_error = -rc;
		return rc;
	}
	int old_fd = f->fd;
	install_fd(f, logger_fd_take(&candidate), &st, day);
	if (logger_fork_close(old_fd))
		return -errno;
	return retention(f, t.tv_sec);
}

static int rotate(logger_file_t *f)
{
	++f->metrics.rotation_attempts;
	int rc = rotate_impl(f);
	if (rc) {
		++f->metrics.rotation_failures;
		f->metrics.last_rotation_error = -rc;
		f->metrics.last_error = -rc;
	} else {
		++f->metrics.rotation_successes;
		f->metrics.last_error = 0;
	}
	return rc;
}

static int prepare_write(logger_file_t *f, size_t n, const struct timespec *t)
{
	if (!f || f->fd < 0)
		return -EBADF;
	if (!t || t->tv_nsec < 0 || t->tv_nsec >= 1000000000 ||
	    n > (size_t)SSIZE_MAX)
		return -EINVAL;
	if (f->detached)
		return -(f->switch_error ? f->switch_error : EIO);
	if (!internal_rotation(f) || !n)
		return 0;
	int size = 0, day = 0;
	if ((f->rotation.mode == LOGGER_ROTATE_SIZE ||
	     f->rotation.mode == LOGGER_ROTATE_SIZE_DAILY) &&
	    f->rotation.max_file_size && f->current_size) {
		size_t max = f->rotation.max_file_size;
		size = f->current_size >= max || n > max - f->current_size;
	}
	if (f->rotation.mode == LOGGER_ROTATE_DAILY ||
	    f->rotation.mode == LOGGER_ROTATE_SIZE_DAILY) {
		int d, rc = wall_day(t->tv_sec, &d);
		if (rc)
			return rc;
		day = d != f->current_day && f->current_size != 0;
	}
	return size || day ? rotate(f) : 0;
}

static int all(logger_file_t *f, int fd, const char *p, size_t n,
	       size_t *written)
{
	*written = 0;
	while (n) {
		++f->metrics.write_syscalls;
		ssize_t rc = logger_fault_should_fail(LOGGER_FAULT_FILE_WRITE) ?
				     -1 :
				     write(fd, p,
					   (size_t)logger_fault_short_write(n));
		if (rc < 0) {
			if (errno == EINTR) {
				++f->metrics.interrupted_write_syscalls;
				continue;
			}
			++f->metrics.failed_write_syscalls;
			return -(errno ? errno : EIO);
		}
		if (!rc) {
			++f->metrics.failed_write_syscalls;
			return -EIO;
		}
		p += rc;
		n -= (size_t)rc;
		*written += (size_t)rc;
	}
	return 0;
}

static int vall(logger_file_t *f, struct iovec *v, int count, size_t *written)
{
	int first = 0;
	*written = 0;
	while (first < count) {
		if (!v[first].iov_len) {
			++first;
			continue;
		}
		int amount = count - first;
		if (amount > f->iov_max)
			amount = f->iov_max;
		++f->metrics.write_syscalls;
		ssize_t rc = writev(f->fd, v + first, amount);
		if (rc < 0) {
			if (errno == EINTR) {
				++f->metrics.interrupted_write_syscalls;
				continue;
			}
			++f->metrics.failed_write_syscalls;
			return -(errno ? errno : EIO);
		}
		if (!rc) {
			++f->metrics.failed_write_syscalls;
			return -EIO;
		}
		*written += (size_t)rc;
		size_t left = (size_t)rc;
		while (first < count && left >= v[first].iov_len) {
			left -= v[first].iov_len;
			++first;
		}
		if (first < count && left) {
			v[first].iov_base = (char *)v[first].iov_base + left;
			v[first].iov_len -= left;
		}
	}
	return 0;
}

static void account(logger_file_t *f, size_t written)
{
	f->current_size = SIZE_MAX - f->current_size < written ?
				  SIZE_MAX :
				  f->current_size + written;
	f->metrics.bytes_written =
		UINT64_MAX - f->metrics.bytes_written < (uint64_t)written ?
			UINT64_MAX :
			f->metrics.bytes_written + (uint64_t)written;
}

int logger_file_write(logger_file_t *f, const char *p, size_t n,
		      const struct timespec *t, int sync)
{
	if (!p && n)
		return -EINVAL;
	if (!f)
		return -EBADF;
	++f->metrics.write_operations;
	int rc = prepare_write(f, n, t);
	size_t written = 0;
	if (!rc)
		rc = all(f, f->fd, p, n, &written);
	account(f, written);
	if (!rc && sync)
		rc = logger_file_sync(f);
	if (rc) {
		++f->metrics.failed_write_operations;
		f->metrics.last_write_error = -rc;
		f->metrics.last_error = -rc;
	} else
		f->metrics.last_error = 0;
	return rc;
}

int logger_file_writev(logger_file_t *f, struct iovec *v, int count, size_t n,
		       const struct timespec *t, int sync)
{
	if (count < 0 || (!v && count))
		return -EINVAL;
	size_t total = 0;
	for (int i = 0; i < count; ++i) {
		if ((!v[i].iov_base && v[i].iov_len) ||
		    v[i].iov_len > (size_t)SSIZE_MAX - total)
			return -EINVAL;
		total += v[i].iov_len;
	}
	if (n != total)
		return -EINVAL;
	if (!f)
		return -EBADF;
	++f->metrics.write_operations;
	int rc = prepare_write(f, n, t);
	size_t written = 0;
	if (!rc)
		rc = vall(f, v, count, &written);
	account(f, written);
	if (!rc && sync)
		rc = logger_file_sync(f);
	if (rc) {
		++f->metrics.failed_write_operations;
		f->metrics.last_write_error = -rc;
		f->metrics.last_error = -rc;
	} else
		f->metrics.last_error = 0;
	return rc;
}
