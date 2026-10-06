#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum proof_failure {
	PROOF_NONE,
	PROOF_QUEUE_COND,
	PROOF_QUEUE_MUTEX,
	PROOF_PROGRESS_COND,
	PROOF_PROGRESS_MUTEX,
	PROOF_EMIT_MUTEX,
	PROOF_CENSUS
};

static enum proof_failure failure;
static logger_t *candidate;
static int candidate_freed;
static int create_failed;
static unsigned release_calls;

void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
	void *p = __real_calloc(count, size);
	if (p && count == 1 && size == sizeof(logger_t) && !candidate)
		candidate = p;
	return p;
}

void __real_free(void *);
void __wrap_free(void *p)
{
	if (p == candidate)
		candidate_freed = 1;
	__real_free(p);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			  void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
			  void *(*start)(void *), void *arg)
{
	if (!create_failed) {
		create_failed = 1;
		return EAGAIN;
	}
	return __real_pthread_create(thread, attr, start, arg);
}

int __real_pthread_cond_destroy(pthread_cond_t *);
int __wrap_pthread_cond_destroy(pthread_cond_t *cv)
{
	if (candidate && failure == PROOF_QUEUE_COND &&
	    cv == &candidate->q.wait_cv) {
		failure = PROOF_NONE;
		return EBUSY;
	}
	if (candidate && failure == PROOF_PROGRESS_COND &&
	    cv == &candidate->progress_cv) {
		failure = PROOF_NONE;
		return EBUSY;
	}
	return __real_pthread_cond_destroy(cv);
}

int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __wrap_pthread_mutex_destroy(pthread_mutex_t *mu)
{
	if (candidate && failure == PROOF_QUEUE_MUTEX &&
	    mu == &candidate->q.wait_mu) {
		failure = PROOF_NONE;
		return EBUSY;
	}
	if (candidate && failure == PROOF_PROGRESS_MUTEX &&
	    mu == &candidate->progress_mu) {
		failure = PROOF_NONE;
		return EBUSY;
	}
	if (candidate && failure == PROOF_EMIT_MUTEX &&
	    mu == &candidate->emit_mu) {
		failure = PROOF_NONE;
		return EBUSY;
	}
	return __real_pthread_mutex_destroy(mu);
}

int __real_logger_process_object_release(void);
int __wrap_logger_process_object_release(void)
{
	++release_calls;
	if (failure == PROOF_CENSUS) {
		failure = PROOF_NONE;
		return -EUCLEAN;
	}
	return __real_logger_process_object_release();
}

static enum proof_failure select_failure(const char *name)
{
	if (!strcmp(name, "success"))
		return PROOF_NONE;
	if (!strcmp(name, "queue-cond"))
		return PROOF_QUEUE_COND;
	if (!strcmp(name, "queue-mutex"))
		return PROOF_QUEUE_MUTEX;
	if (!strcmp(name, "progress-cond"))
		return PROOF_PROGRESS_COND;
	if (!strcmp(name, "progress-mutex"))
		return PROOF_PROGRESS_MUTEX;
	if (!strcmp(name, "emit-mutex"))
		return PROOF_EMIT_MUTEX;
	if (!strcmp(name, "census"))
		return PROOF_CENSUS;
	return -1;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	failure = select_failure(argv[1]);
	CHECK((int)failure >= 0);

	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_STDERR;
	c.async_mode = 1;
	c.queue_capacity = 64;

	errno = 0;
	logger_t *l = logger_create(&c);
	CHECK(l == NULL);
	CHECK(errno == EAGAIN);
	CHECK(create_failed == 1);
	CHECK(candidate != NULL);

	if (!strcmp(argv[1], "success")) {
		CHECK(candidate_freed == 1);
		CHECK(release_calls == 1);
		CHECK(logger_process_object_count() == 0);
		return 0;
	}

	/* proof failure turns the unpublished object into an intentional tombstone:
	 * no final free and no census release that could make fork state lie. */
	CHECK(candidate_freed == 0);
	CHECK(atomic_load_explicit(&candidate->lifecycle_error,
				   memory_order_acquire) ==
	      (!strcmp(argv[1], "census") ? EUCLEAN : EBUSY));
	CHECK(logger_process_object_count() == 1);
	if (!strcmp(argv[1], "census"))
		CHECK(release_calls == 1);
	else
		CHECK(release_calls == 0);
	return 0;
}
