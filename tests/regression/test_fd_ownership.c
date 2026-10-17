#define _GNU_SOURCE
#include "logger_fd.h"
#include "logger.h"
#include "support.h"
#include <fcntl.h>

/* 只包装测试程序的系统调用，生产头文件不含测试钩子。 */
static unsigned close_calls;
static unsigned directory_calls;
static unsigned closedir_calls;
static int close_error;
static int directory_error;
static int reuse_fd;
static struct logger_owned_fd *closing_owner;

int __real_close(int);
DIR *__real_fdopendir(int);
int __real_closedir(DIR *);

int __wrap_close(int fd)
{
	++close_calls;
	if (closing_owner)
		CHECK(!logger_fd_valid(closing_owner));
	CHECK(!__real_close(fd));
	if (reuse_fd) {
		int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);

		CHECK(replacement >= 0);
		if (replacement != fd) {
			CHECK(dup2(replacement, fd) == fd);
			CHECK(!__real_close(replacement));
		}
	}
	if (close_error) {
		errno = close_error;
		return -1;
	}
	return 0;
}

DIR *__wrap_fdopendir(int fd)
{
	++directory_calls;
	CHECK(fcntl(fd, F_GETFD) >= 0);
	if (directory_error) {
		errno = directory_error;
		return NULL;
	}
	return __real_fdopendir(fd);
}

int __wrap_closedir(DIR *directory)
{
	++closedir_calls;
	return __real_closedir(directory);
}

static int new_fd(void)
{
	int fd = logger_fork_open("/dev/null", O_RDONLY | O_CLOEXEC, 0);

	CHECK(fd >= 0);
	return fd;
}

static void close_once(void)
{
	struct logger_owned_fd fd = logger_fd_own(new_fd());
	struct logger_borrowed_fd view = logger_fd_borrow(&fd);
	int raw = logger_fd_number(view);

	closing_owner = &fd;
	CHECK(fcntl(raw, F_GETFD) >= 0);
	CHECK(!logger_fd_close_status(&fd));
	CHECK(!logger_fd_close_status(&fd));
	logger_fd_cleanup(&fd);
	CHECK(close_calls == 1);
	CHECK(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
}

static void take_once(void)
{
	int raw;
	{
		struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
			logger_fd_own(new_fd());

		raw = logger_fd_take(&fd);
		CHECK(!logger_fd_valid(&fd));
		CHECK(logger_fd_take(&fd) == -1);
	}
	CHECK(close_calls == 0);
	CHECK(fcntl(raw, F_GETFD) >= 0);
	/* take 仅移交所有权，最终关闭仍必须消费 fork 位图登记。 */
	CHECK(!logger_fork_close(raw));
	CHECK(close_calls == 1);
	CHECK(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
	CHECK(logger_fork_close(raw) == -1 && errno == EBADF);

	/* 不经裸 int 往返，直接将拥有者移入另一私有字段。 */
	struct logger_owned_fd source = logger_fd_own(new_fd());
	struct logger_owned_fd destination = logger_fd_move(&source);
	CHECK(!logger_fd_valid(&source) && logger_fd_valid(&destination));
	int moved = logger_fd_number(logger_fd_borrow(&destination));
	CHECK(!logger_fd_close_status(&source));
	CHECK(!logger_fd_close_status(&destination));
	CHECK(fcntl(moved, F_GETFD) == -1 && errno == EBADF);
	CHECK(close_calls == 2);
}

static int scope_error(void)
{
	struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(new_fd());

	CHECK(logger_fd_valid(&fd));
	close_error = EIO;
	errno = EACCES;
	return -EACCES;
}

static void close_reuse(int error)
{
	struct logger_owned_fd fd = logger_fd_own(new_fd());
	int raw = logger_fd_number(logger_fd_borrow(&fd));

	closing_owner = &fd;
	close_error = error;
	reuse_fd = 1;
	CHECK(logger_fd_close_status(&fd) == -error);
	CHECK(errno == error);
	CHECK(!logger_fd_valid(&fd));
	CHECK(!logger_fd_close_status(&fd));
	logger_fd_cleanup(&fd);
	CHECK(close_calls == 1);
	/* 原整数已被另一个真实描述符占用；旧拥有者不能再关闭它。 */
	CHECK(fcntl(raw, F_GETFD) >= 0);
	CHECK(!__real_close(raw));
}

static void directory_transfer(int fail, int retry)
{
	int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
	int raw = logger_fork_open(".", flags, 0);
	DIR *directory;

	CHECK(raw >= 0);
	{
		struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
			logger_fd_own(raw);

		directory_error = fail ? ENOMEM : 0;
		directory = logger_fd_fdopendir(&fd);
		if (fail) {
			CHECK(!directory && errno == ENOMEM);
			CHECK(logger_fd_valid(&fd));
			CHECK(logger_fd_number(logger_fd_borrow(&fd)) == raw);
			CHECK(fcntl(raw, F_GETFD) >= 0);
		}
		if (retry) {
			directory_error = 0;
			directory = logger_fd_fdopendir(&fd);
		}
		if (!fail || retry) {
			CHECK(directory);
			CHECK(!logger_fd_valid(&fd));
			CHECK(dirfd(directory) == raw);
		}
	}
	if (directory) {
		CHECK(close_calls == 0);
		CHECK(fcntl(raw, F_GETFD) >= 0);
		CHECK(!logger_fork_closedir(directory));
		CHECK(closedir_calls == 1);
	} else {
		CHECK(close_calls == 1);
		CHECK(closedir_calls == 0);
		CHECK(errno == ENOMEM);
	}
	CHECK(directory_calls == (retry ? 2u : 1u));
	CHECK(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
}

static void invalid_owner(void)
{
	struct logger_owned_fd fd LOGGER_CLEANUP(logger_fd_cleanup) =
		logger_fd_own(-1);

	CHECK(!logger_fd_valid(&fd));
	CHECK(logger_fd_take(&fd) == -1);
	CHECK(!logger_fd_close_status(&fd));
	CHECK(!logger_fd_fdopendir(&fd) && errno == EBADF);
	CHECK(close_calls == 0 && directory_calls == 0);
}

static void zero_descriptor(void)
{
	int saved = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
	CHECK(!__real_close(STDIN_FILENO));
	/* 必须经过 fork 资源登记，而非从宿主 dup2 偷偷制造已登记 fd。 */
	int raw = new_fd();
	CHECK(raw == STDIN_FILENO);
	struct logger_owned_fd fd = logger_fd_own(raw);

	CHECK(logger_fd_valid(&fd));
	CHECK(!logger_fd_close_status(&fd));
	CHECK(close_calls == 1);
	CHECK(fcntl(STDIN_FILENO, F_GETFD) == -1 && errno == EBADF);
	if (saved >= 0) {
		CHECK(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
		CHECK(!__real_close(saved));
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(logger_process_init() == 0);
	if (!strcmp(argv[1], "close"))
		close_once();
	else if (!strcmp(argv[1], "take"))
		take_once();
	else if (!strcmp(argv[1], "cleanup-error")) {
		CHECK(scope_error() == -EACCES);
		CHECK(errno == EACCES && close_calls == 1);
	} else if (!strcmp(argv[1], "close-eintr-reuse"))
		close_reuse(EINTR);
	else if (!strcmp(argv[1], "close-eio-reuse"))
		close_reuse(EIO);
	else if (!strcmp(argv[1], "directory-success"))
		directory_transfer(0, 0);
	else if (!strcmp(argv[1], "directory-failure"))
		directory_transfer(1, 0);
	else if (!strcmp(argv[1], "directory-retry"))
		directory_transfer(1, 1);
	else if (!strcmp(argv[1], "invalid"))
		invalid_owner();
	else if (!strcmp(argv[1], "zero"))
		zero_descriptor();
	else
		CHECK(!"unknown fd ownership scenario");
	printf("FD_OWNERSHIP_PASS=%s\n", argv[1]);
	return 0;
}
