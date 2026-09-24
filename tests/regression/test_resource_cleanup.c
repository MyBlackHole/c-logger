#define _POSIX_C_SOURCE 200809L
#include "logger_cleanup.h"
#include "support.h"

#include <fcntl.h>
#include <stdatomic.h>

static _Atomic unsigned free_calls;

void __real_free(void *);
void __wrap_free(void *p)
{
	if (p)
		atomic_fetch_add_explicit(&free_calls, 1, memory_order_relaxed);
	__real_free(p);
}

static void auto_free_scope(void)
{
	char *buffer LOGGER_AUTO_FREE = malloc(64);
	CHECK(buffer);
	buffer[0] = 'x';
}

static char *take_ptr_scope(void)
{
	char *buffer LOGGER_AUTO_FREE = malloc(64);
	CHECK(buffer);
	buffer[0] = 'y';
	char *owned = LOGGER_TAKE_PTR(buffer);
	CHECK(!buffer && owned && owned[0] == 'y');
	return owned;
}

static int auto_fd_scope(void)
{
	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	int fd LOGGER_AUTO_FD = pipefd[0];
	CHECK(close(pipefd[1]) == 0);
	errno = E2BIG;
	return fd;
}

static int take_fd_scope(void)
{
	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	int fd LOGGER_AUTO_FD = pipefd[0];
	CHECK(close(pipefd[1]) == 0);
	int owned = LOGGER_TAKE_FD(fd);
	CHECK(fd == -1);
	return owned;
}

static int auto_file_scope(void)
{
	FILE *file LOGGER_AUTO_FILE = tmpfile();
	CHECK(file);
	int fd = fileno(file);
	CHECK(fd >= 0);
	errno = ENOTTY;
	return fd;
}

int main(void)
{
	unsigned before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	auto_free_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) ==
	      before + 1);

	before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	char *owned = take_ptr_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) == before);
	free(owned);
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) ==
	      before + 1);

	errno = 0;
	int fd = auto_fd_scope();
	CHECK(errno == E2BIG);
	errno = 0;
	CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);

	fd = take_fd_scope();
	CHECK(fcntl(fd, F_GETFD) != -1);
	CHECK(close(fd) == 0);

	errno = 0;
	fd = auto_file_scope();
	CHECK(errno == ENOTTY);
	errno = 0;
	CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);

	puts("internal lexical cleanup and ownership transfer passed");
	return 0;
}
