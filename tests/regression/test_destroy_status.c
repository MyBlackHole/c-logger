#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include <fcntl.h>
static int fail_close, fail_join, fail_cond_destroy, fail_mutex_destroy;
static int selected_fd = -1;
static logger_t *subject;

int __real_close(int);
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (fd == selected_fd && fail_close) {
		fail_close = 0;
		errno = EIO;
		return -1;
	}
	return rc;
}

int __real_pthread_join(pthread_t, void **);
int __wrap_pthread_join(pthread_t thread, void **result)
{
	if (fail_join) {
		fail_join = 0;
		/* Let the real worker quiesce so the test itself has no runaway
		 * thread, but report a failed lifetime proof to the implementation.
		 * The implementation must conservatively refuse final free because
		 * it cannot rely on facts hidden behind a failed pthread_join(). */
		int rc = __real_pthread_join(thread, result);
		return rc ? rc : EDEADLK;
	}
	return __real_pthread_join(thread, result);
}

int __real_pthread_cond_destroy(pthread_cond_t *);
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (subject && cv == &subject->progress_cv && fail_cond_destroy) {
		fail_cond_destroy = 0;
		return EBUSY;
	}
	return __real_pthread_cond_destroy(cv);
}

int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __wrap_pthread_mutex_destroy(pthread_mutex_t *mu)
{
	if (subject && mu == &subject->progress_mu && fail_mutex_destroy) {
		fail_mutex_destroy = 0;
		return EBUSY;
	}
	return __real_pthread_mutex_destroy(mu);
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	unlink("dispose.log");
	unlink("next.log");
	if (!strcmp(argv[1], "null")) {
		CHECK(logger_destroy_status(NULL) == 0);
		return 0;
	}
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "dispose.log";
	c.async_mode = 1;
	c.queue_capacity = 2048;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	int io_error = !strcmp(argv[1], "io-error");
	if (io_error) {
		c.file_path = "/dev/full";
		c.async_mode = 0;
	}
	logger_t *l = logger_create(&c);
	CHECK(l != NULL);
	subject = l;
	if (!strcmp(argv[1], "join-error"))
		fail_join = 1;
	else if (!strcmp(argv[1], "cond-destroy-error"))
		fail_cond_destroy = 1;
	else if (!strcmp(argv[1], "mutex-destroy-error"))
		fail_mutex_destroy = 1;
	if (!strcmp(argv[1], "close-error")) {
		selected_fd = l->file_backend.fd;
		fail_close = 1;
	}
	int lifecycle_error = fail_join || fail_cond_destroy || fail_mutex_destroy;
	int expected = io_error ? ENOSPC :
		       fail_close ? EIO :
		       fail_join ? EDEADLK :
		       (fail_cond_destroy || fail_mutex_destroy) ? EBUSY : 0;
	for (int i = 0; i < (io_error ? 1 : 1000); ++i)
		LOGGER_INFO(l, "test", "record=%d", i);
	int rc = logger_destroy_status(l);
	l = NULL; /* destroy consumes public ownership on every return */
	CHECK(expected ? rc == -1 && errno == expected : rc == 0);
	if (lifecycle_error) {
		/* Lifetime proof failed: the implementation must intentionally retain
		 * the object/resources instead of freeing possibly reachable memory. */
		CHECK(logger_process_object_count() == 1);
		return 0;
	}
	CHECK(logger_process_object_count() == 0);
	if (selected_fd >= 0)
		CHECK(fcntl(selected_fd, F_GETFD) == -1 && errno == EBADF);
	if (!io_error)
		CHECK(log_contains("dispose.log", "record=999"));
	subject = NULL;
	c.file_path = "next.log";
	c.async_mode = 0;
	l = logger_create(&c);
	CHECK(l != NULL);
	CHECK(logger_destroy_status(l) == 0);
	return 0;
}
