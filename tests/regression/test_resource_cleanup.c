#define _POSIX_C_SOURCE 200809L
#include "logger_cleanup.h"
#include "support.h"

#include <fcntl.h>
#include <stdatomic.h>

static _Atomic unsigned free_calls;
static int class_release_total;

void __real_free(void *);
void __wrap_free(void *p)
{
	if (p)
		atomic_fetch_add_explicit(&free_calls, 1, memory_order_relaxed);
	__real_free(p);
}

static void auto_free_scope(void)
{
	char *buffer __free(free) = malloc(64);
	CHECK(buffer);
	buffer[0] = 'x';
}

static char *no_free_scope(void)
{
	char *buffer __free(free) = malloc(64);
	CHECK(buffer);
	buffer[0] = 'y';
	char *owned = no_free_ptr(buffer);
	CHECK(!buffer && owned && owned[0] == 'y');
	return owned;
}

static char *return_ptr_scope(void)
{
	char *buffer __free(free) = malloc(64);
	CHECK(buffer);
	buffer[0] = 'z';
	return_ptr(buffer);
}

static int consume_ptr(char *buffer)
{
	CHECK(buffer);
	return 0;
}

static void retain_scope(void)
{
	char *buffer __free(free) = malloc(64);
	CHECK(buffer);
	if (consume_ptr(buffer) == 0)
		retain_and_null_ptr(buffer);
	CHECK(!buffer);
}

static int auto_fd_scope(void)
{
	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	int fd __free(close_fd) = pipefd[0];
	CHECK(close(pipefd[1]) == 0);
	errno = E2BIG;
	return fd;
}

static int take_fd_scope(void)
{
	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	CLASS(fd, owned)(pipefd[0]);
	CHECK(close(pipefd[1]) == 0);
	int fd = take_fd(owned);
	CHECK(owned == -1);
	return fd;
}

static int auto_file_scope(void)
{
	CLASS(file, file)(tmpfile());
	CHECK(file);
	int fd = fileno(file);
	CHECK(fd >= 0);
	errno = ENOTTY;
	return fd;
}

DEFINE_CLASS(test_resource, int,
	     if (_T > 0) class_release_total += _T,
	     value, int value)

static void fake_lock(int *state)
{
	CHECK(*state == 0);
	*state = 1;
}

static void fake_unlock(int *state)
{
	CHECK(*state == 1);
	*state = 0;
}

static int fake_trylock(int *state)
{
	if (*state)
		return EBUSY;
	*state = 1;
	return 0;
}

DEFINE_GUARD(test_lock, int *, fake_lock(_T), fake_unlock(_T))
DEFINE_GUARD_COND(test_lock, _try, fake_trylock(_T))

static void class_and_guard_scope(void)
{
	{
		CLASS(test_resource, resource)(3);
		CHECK(resource == 3);
	}
	CHECK(class_release_total == 3);

	scoped_class(test_resource, resource, 4) {
		CHECK(resource == 4);
	}
	CHECK(class_release_total == 7);

	int lock = 0;
	{
		guard(test_lock)(&lock);
		CHECK(lock == 1);
	}
	CHECK(lock == 0);

	scoped_guard(test_lock, &lock) {
		CHECK(lock == 1);
	}
	CHECK(lock == 0);

	{
		ACQUIRE(test_lock_try, acquired)(&lock);
		CHECK(ACQUIRE_ERR(test_lock_try, &acquired) == 0);
		CHECK(lock == 1);
	}
	CHECK(lock == 0);

	lock = 1;
	{
		ACQUIRE(test_lock_try, busy)(&lock);
		CHECK(ACQUIRE_ERR(test_lock_try, &busy) == -EBUSY);
	}
	CHECK(lock == 1);

	int entered = 0;
	scoped_guard(test_lock_try, &lock) {
		entered = 1;
	}
	CHECK(!entered && lock == 1);

	int failed = 0;
	scoped_cond_guard(test_lock_try, failed = 1, &lock) {
		CHECK(!"busy conditional guard entered");
	}
	CHECK(failed == 1 && lock == 1);
	lock = 0;
}

int main(void)
{
	unsigned before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	auto_free_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) ==
	      before + 1);

	before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	char *owned = no_free_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) == before);
	free(owned);
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) ==
	      before + 1);

	before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	owned = return_ptr_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) == before);
	free(owned);
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) ==
	      before + 1);

	before = atomic_load_explicit(&free_calls, memory_order_relaxed);
	retain_scope();
	CHECK(atomic_load_explicit(&free_calls, memory_order_relaxed) == before);
	/* consume_ptr models ownership transfer; release it explicitly here. */
	/* A real consuming API would own the resource after success. */

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

	class_and_guard_scope();

	puts("linux-style cleanup ownership/class/guard semantics passed");
	return 0;
}
