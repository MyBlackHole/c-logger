#define _GNU_SOURCE
#include "logger_file.h"
#include "logger_fault.h"
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
	if (!localtime_r(&sec, &t) || t.tm_year < -1900 || t.tm_year > 8099)
		return -EOVERFLOW;
	*day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
	return 0;
}

static int sync_data(int fd)
{
	int rc;
	do {
		rc = logger_fault_should_fail(LOGGER_FAULT_FILE_FSYNC) ?
			     -1 :
			     fsync(fd);
	} while (rc < 0 && errno == EINTR);
	return rc < 0 ? -errno : 0;
}

static int sync_directory(logger_file_t *f)
{
	int rc;
	do {
		rc = fsync(f->dir_fd);
	} while (rc < 0 && errno == EINTR);
	if (rc < 0)
		return -errno;
	f->dir_dirty = 0;
	return 0;
}

int logger_file_close_status(logger_file_t *f)
{
	if (!f)
		return -EINVAL;
	int error = 0;
	int *fds[] = { &f->fd, &f->lock_fd, &f->dir_fd };
	/* Keep ownership through data-fd close. No explicit LOCK_UN: fork/dup
     * references share a flock; close only this owner's references. */
	for (unsigned i = 0; i < sizeof(fds) / sizeof(*fds); ++i) {
		int fd = *fds[i];
		*fds[i] = -1;
		if (fd >= 0 && close(fd) < 0 && !error)
			error = errno;
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
	memcpy(f->name, base, n + 1);
	char dir[LOGGER_PATH_MAX];
	if (slash) {
		size_t dlen = slash == path ? 1u : (size_t)(slash - path);
		memcpy(dir, path, dlen);
		dir[dlen] = 0;
	} else
		memcpy(dir, ".", 2);
	f->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
				0; /* /dev/null and /dev/full for plumbing, NOT durability */
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

int logger_file_reserve(logger_file_t *f, const char *path,
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
		f->lock_fd = openat(f->dir_fd, f->lock_name,
				    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
					    O_NONBLOCK,
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

/* No file is truncated. O_NONBLOCK makes a substituted FIFO fail validation
 * rather than hanging open(). Directory ancestors must be trusted. */
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
	int fd = openat(f->dir_fd, f->name, flags, f->mode);
	if (fd < 0)
		return -errno;
	rc = check_named_fd(f->dir_fd, f->name, fd, st);
	if (!rc)
		rc = f->managed ? regular_single_link(st) :
				  (S_ISCHR(st->st_mode) ? 0 : -EINVAL);
	if (rc) {
		(void)close(fd);
		return rc;
	}
	*out = fd;
	/* A previously absent active may have been created. Conservatively sync
     * the bound directory even if it existed; this never depends on cwd. */
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

int logger_file_open_reserved(logger_file_t *f)
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
	int rc = sync_data(f->fd);
	if (!rc && f->dir_dirty)
		rc = sync_directory(f);
	if (!rc && f->detached)
		rc = -(f->switch_error ? f->switch_error : EIO);
	return rc;
}

int logger_file_reopen(logger_file_t *f)
{
	if (!f || f->fd < 0) {
		errno = EBADF;
		return -1;
	}
	int day = 0, fd = -1;
	struct stat st;
	int rc = wall_day(time(NULL), &day);
	if (!rc)
		rc = open_candidate(f, 0, &fd, &st);
	/* Candidate errors do not retire the working fd. Sync the old segment
     * before switching; neither an open error nor fsync error erases it. */
	if (!rc && f->managed)
		rc = sync_data(f->fd);
	if (!rc && f->dir_dirty)
		rc = sync_directory(f);
	if (rc) {
		if (fd >= 0)
			(void)close(fd);
		errno = -rc;
		return -1;
	}
	int old_fd = f->fd;
	install_fd(f, fd, &st, day);
	if (close(old_fd) < 0)
		return -1; /* new fd installed; do not retry close */
	return 0;
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
	       gmtime_r(&t.tv_sec, &tm) && tm.tm_year >= -1900 &&
	       tm.tm_year <= 8099;
}

static int advance_microsecond(struct timespec *t)
{
	if (t->tv_nsec < 999999000L)
		t->tv_nsec += 1000;
	else {
		/* Do not overflow time_t on 32-bit targets. */
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
	if (!timestamp_valid(*t) || !gmtime_r(&t->tv_sec, &tm) ||
	    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &tm) != 15u)
		return -EOVERFLOW;
	int n = snprintf(out, cap, "%s.%s.%06ldZ%s", f->stem, stamp,
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
	/* A separate directory description avoids sharing readdir's offset with
     * the bound directory or an Audit scanner. */
	int fd = openat(f->dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	DIR *d = fdopendir(fd);
	if (!d) {
		int rc = -errno;
		(void)close(fd);
		return rc;
	}
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
		/* difftime avoids signed time_t subtraction overflow. */
		if (difftime(now, st.st_mtime) <= (double)seconds)
			continue;
		if (unlinkat(f->dir_fd, entry->d_name, 0)) {
			rc = -errno;
			break;
		}
		changed = 1;
	}
	if (closedir(d) && !rc)
		rc = -errno;
	if (changed) {
		f->dir_dirty = 1;
		int synced = sync_directory(f);
		if (!rc)
			rc = synced;
	}
	return rc;
}

static int rotate(logger_file_t *f)
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
	/* The old fd now belongs to an archive. Never append to it, even if
     * opening/syncing the new active fails. Only explicit reopen may recover. */
	f->detached = 1;
	f->switch_error = EIO;
	f->dir_dirty = 1;
	f->archive_time = t;
	f->archive_time_valid = 1;
	logger_fault_crash_if_requested("file_after_archive_rename");
	rc = sync_directory(f);
	if (!rc)
		logger_fault_crash_if_requested("file_after_archive_dirsync");
	int candidate = -1, day = 0;
	struct stat st;
	if (!rc)
		rc = wall_day(t.tv_sec, &day);
	if (!rc)
		rc = open_candidate(f, 1, &candidate, &st);
	if (!rc)
		logger_fault_crash_if_requested("file_after_active_open");
	if (!rc)
		rc = sync_data(
			candidate); /* persist the new empty inode first */
	if (!rc)
		rc = sync_directory(f);
	if (!rc)
		logger_fault_crash_if_requested("file_after_active_dirsync");
	if (rc) {
		if (candidate >= 0)
			(void)close(candidate);
		f->switch_error = -rc;
		return rc;
	}
	int old_fd = f->fd;
	install_fd(f, candidate, &st, day);
	if (close(old_fd))
		return -errno;
	return retention(f, t.tv_sec);
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

static int all(int fd, const char *p, size_t n, size_t *written)
{
	*written = 0;
	while (n) {
		ssize_t rc = logger_fault_should_fail(LOGGER_FAULT_FILE_WRITE) ?
				     -1 :
				     write(fd, p,
					   (size_t)logger_fault_short_write(n));
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!rc)
			return -EIO;
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
		ssize_t rc = writev(f->fd, v + first, amount);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!rc)
			return -EIO;
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
}

int logger_file_write(logger_file_t *f, const char *p, size_t n,
		      const struct timespec *t, int sync)
{
	if (!p && n)
		return -EINVAL;
	int rc = prepare_write(f, n, t);
	if (rc)
		return rc;
	size_t written;
	rc = all(f->fd, p, n, &written);
	account(f, written);
	if (!rc && sync)
		rc = logger_file_sync(f);
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
	int rc = prepare_write(f, n, t);
	if (rc)
		return rc;
	size_t written;
	rc = vall(f, v, count, &written);
	account(f, written);
	if (!rc && sync)
		rc = logger_file_sync(f);
	return rc;
}

int logger_file_offset_get(logger_file_t *f, uint64_t *out)
{
	if (!f || !out) {
		errno = EINVAL;
		return -1;
	}
	if (f->detached) {
		errno = f->switch_error ? f->switch_error : EIO;
		return -1;
	}
	off_t x = lseek(f->fd, 0, SEEK_END);
	if (x < 0)
		return -1;
	*out = (uint64_t)x;
	return 0;
}
