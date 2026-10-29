#define _GNU_SOURCE
#include "logger.h"
#include "logger_file.h"
#include "support.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>

/*
 * 仅测试可执行文件包装生产归档里的实际资源取得/释放调用。
 * 模拟失败不构造假的 fd/DIR，也不触碰非法 pthread 对象。
 */
enum fault {
	FAULT_NONE,
	FAULT_BIND_DIRECTORY,
	FAULT_LOCK_FILE,
	FAULT_RETENTION_DIRECTORY,
	FAULT_FDOPENDIR
};

struct descriptor {
	int fd;
	int released;
};

static enum fault selected;
static int failed, retention_fd = -1;
static unsigned int directory_opens, lock_opens, retention_opens;
static unsigned int fdopendir_calls, closedir_calls, release_calls;
static struct descriptor descriptors[64];
static unsigned int acquired;

static void got_fd(int fd)
{
	if (fd < 0)
		return;
	CHECK(acquired < sizeof(descriptors) / sizeof(descriptors[0]));
	for (unsigned int i = 0; i < acquired; ++i)
		CHECK(descriptors[i].released || descriptors[i].fd != fd);
	descriptors[acquired++] = (struct descriptor){ .fd = fd };
}

static int tracked_live_fd(int fd)
{
	for (unsigned int i = 0; i < acquired; ++i)
		if (descriptors[i].fd == fd && !descriptors[i].released)
			return 1;
	return 0;
}

static void released_fd(int fd)
{
	for (unsigned int i = acquired; i > 0; --i) {
		struct descriptor *entry = &descriptors[i - 1];
		if (entry->fd == fd && !entry->released) {
			entry->released = 1;
			++release_calls;
			return;
		}
	}
	CHECK(!"closing an untracked or already released fd");
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	CHECK(!(flags & O_CREAT)); /* 当前绑定目录仅使用 O_DIRECTORY。 */
	if (flags & O_DIRECTORY) {
		++directory_opens;
		if (selected == FAULT_BIND_DIRECTORY && !failed) {
			++failed;
			errno = EACCES;
			return -1;
		}
	}
	int fd = __real_open(path, flags);
	got_fd(fd);
	return fd;
}

int __real_openat(int dirfd, const char *name, int flags, ...);
int __wrap_openat(int dirfd, const char *name, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	int is_lock = strstr(name, ".logger.lock") != NULL;
	int is_retention = !strcmp(name, ".") && (flags & O_DIRECTORY);
	if (is_lock) {
		++lock_opens;
		if (selected == FAULT_LOCK_FILE && !failed) {
			++failed;
			errno = EMFILE;
			return -1;
		}
	}
	if (is_retention) {
		++retention_opens;
		if (selected == FAULT_RETENTION_DIRECTORY && !failed) {
			++failed;
			errno = EMFILE;
			return -1;
		}
	}
	int fd = __real_openat(dirfd, name, flags, mode);
	got_fd(fd);
	if (is_retention && fd >= 0)
		retention_fd = fd;
	return fd;
}

DIR *__real_fdopendir(int fd);
DIR *__wrap_fdopendir(int fd)
{
	++fdopendir_calls;
	CHECK(fd == retention_fd);
	CHECK(fcntl(fd, F_GETFD) >= 0);
	if (selected == FAULT_FDOPENDIR && !failed) {
		++failed;
		errno = ENOMEM;
		return NULL; /* 未接管 fd，原属主的 logger_fd_cleanup 回收。 */
	}
	return __real_fdopendir(fd);
}

int __real_close(int fd);
int __wrap_close(int fd)
{
	released_fd(fd);
	return __real_close(fd);
}

int __real_closedir(DIR *directory);
int __wrap_closedir(DIR *directory)
{
	int fd = dirfd(directory);
	CHECK(fd >= 0);
	int rc = __real_closedir(directory);
	if (!rc && tracked_live_fd(fd)) {
		++closedir_calls;
		/* libc closedir 内部 close 不一定会经过 --wrap。 */
		released_fd(fd);
	}
	return rc;
}

static unsigned int open_descriptor_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	unsigned int count = 0;
	struct dirent *entry;
	while ((entry = readdir(d)))
		if (entry->d_name[0] != '.')
			++count;
	CHECK(!closedir(d));
	return count;
}

static void all_descriptors_released(unsigned int baseline)
{
	for (unsigned int i = 0; i < acquired; ++i)
		CHECK(descriptors[i].released);
	CHECK(acquired == release_calls);
	CHECK(open_descriptor_count() == baseline);
}

static void two_writes(struct logger_file *f, int second_error)
{
	struct timespec stamp = { 1700000000, 0 };
	CHECK(!logger_file_write(f, "a", 1, &stamp, 1));
	CHECK(f->current_size == 1);
	int rc = logger_file_write(f, "b", 1, &stamp, 1);
	CHECK(rc == -second_error);
	if (second_error) {
		CHECK(f->metrics.rotation_failures == 1);
		CHECK(f->metrics.last_rotation_error == second_error);
		CHECK(!f->detached);
		CHECK(logger_fd_valid(&f->fd));
	} else
		CHECK(f->metrics.rotation_successes == 1);
}

int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	if (!strcmp(argv[1], "healthy"))
		selected = FAULT_NONE;
	else if (!strcmp(argv[1], "bind-directory"))
		selected = FAULT_BIND_DIRECTORY;
	else if (!strcmp(argv[1], "lock-file"))
		selected = FAULT_LOCK_FILE;
	else if (!strcmp(argv[1], "retention-directory"))
		selected = FAULT_RETENTION_DIRECTORY;
	else if (!strcmp(argv[1], "fdopendir"))
		selected = FAULT_FDOPENDIR;
	else
		CHECK(!"unknown file resource acquisition scenario");

	unsigned int baseline = open_descriptor_count();
	logger_rotation_config_t rotation = { 0 };
	rotation.mode = LOGGER_ROTATE_SIZE;
	rotation.max_file_size = 1;
	rotation.retention_days = 1;
	struct logger_file file = LOGGER_FILE_EMPTY;
	if (selected == FAULT_BIND_DIRECTORY || selected == FAULT_LOCK_FILE) {
		int expected = EMFILE;
		if (selected == FAULT_BIND_DIRECTORY)
			expected = EACCES;
		errno = 0;
		CHECK(logger_file_init(&file, "out.log", rotation, 0600) == -1);
		CHECK(errno == expected && failed == 1);
		CHECK(!logger_fd_valid(&file.fd) &&
		      !logger_fd_valid(&file.dir_fd) &&
		      !logger_fd_valid(&file.lock_fd));
		all_descriptors_released(baseline);
		selected = FAULT_NONE;
	}

	CHECK(!logger_file_init(&file, "out.log", rotation, 0600));
	CHECK(logger_fd_valid(&file.fd) && logger_fd_valid(&file.dir_fd) &&
	      logger_fd_valid(&file.lock_fd));
	if (selected == FAULT_RETENTION_DIRECTORY ||
	    selected == FAULT_FDOPENDIR) {
		int expected = selected == FAULT_FDOPENDIR ? ENOMEM : EMFILE;
		two_writes(&file, expected);
		CHECK(failed == 1);
		CHECK(retention_opens == 1);
		CHECK(fdopendir_calls ==
		      (selected == FAULT_FDOPENDIR ? 1u : 0u));
		if (selected == FAULT_FDOPENDIR)
			CHECK(fcntl(retention_fd, F_GETFD) == -1 &&
			      errno == EBADF);
		selected = FAULT_NONE;
		struct timespec stamp = { 1700000000, 0 };
		CHECK(!logger_file_write(&file, "c", 1, &stamp, 1));
		/*
		 * 失败的轮转已经装入新的空活动 fd。先写满，再触发
		 * 一次正常轮转，验证 retention 及 DIR 所有权恢复。
		 */
		CHECK(!logger_file_write(&file, "d", 1, &stamp, 1));
	} else
		two_writes(&file, 0);
	CHECK(!logger_file_close_status(&file));
	all_descriptors_released(baseline);
	CHECK(directory_opens >= 1);
	CHECK(lock_opens >= 1);
	CHECK(retention_opens >= 1);
	CHECK(fdopendir_calls >= 1);
	CHECK(closedir_calls >= 1);
	if (strcmp(argv[1], "healthy"))
		CHECK(failed == 1);
	puts("file acquisition failure regression passed");
	return 0;
}
