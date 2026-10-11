#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "logger_cleanup.h"
#include "support.h"
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

static int a, b, c;
static pthread_mutex_t real_mutex;
static pthread_mutex_t real_control_mutex;
static pthread_mutex_t real_lifetime_mutex;
static pthread_mutex_t real_abba_a;
static pthread_mutex_t real_abba_b;
static pthread_barrier_t real_abba_ready;
static pthread_mutex_t real_trylock_mutex;
static pthread_barrier_t real_trylock_ready;
static pthread_barrier_t real_trylock_done;
static pthread_rwlock_t real_rwlock;
static pthread_barrier_t real_rwlock_ready;
static pthread_barrier_t real_rwlock_done;
static pthread_mutex_t tls_mutex_a;
static pthread_mutex_t tls_mutex_b;
static pthread_barrier_t tls_ready;
static pthread_barrier_t tls_done;
static pthread_mutex_t cond_mutex;
static pthread_cond_t cond_cv;
static int cond_ready;
static _Atomic unsigned *unlock_calls;
static pthread_mutex_t *unlock_probe_target;
static int unlock_probe_armed;

int __real_pthread_mutex_unlock(pthread_mutex_t *);

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (unlock_probe_armed && mu == unlock_probe_target)
		atomic_fetch_add_explicit(unlock_calls, 1, memory_order_relaxed);
	return __real_pthread_mutex_unlock(mu);
}

static void expect_abort(void (*fn)(void))
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		alarm(3);
		fn();
		_exit(0);
	}
	int status;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFSIGNALED(status));
	CHECK(WTERMSIG(status) == SIGABRT);
}

static void bad_instance_nesting(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_PROGRESS, &b);
}

static void bad_reverse_global(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
}

static void bad_release(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);
}

static void bad_assert(void)
{
	logger_lockdep_assert_held(LOGGER_LOCK_QUEUE_WAIT, &a);
}

static void preflight_checks_do_not_change_state(void)
{
	logger_lockdep_check_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_check_release(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_release(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_EMIT, &a);
}

static void bad_preflight_recursive_acquire(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_check_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
}

static void bad_preflight_lock_order(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &a);
	logger_lockdep_check_acquire(LOGGER_LOCK_INSTANCE_PROGRESS, &b);
}

static void bad_preflight_release_order(void)
{
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_check_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);
}

static void init_normal_mutex(pthread_mutex_t *mu)
{
	pthread_mutexattr_t attr;
	CHECK(pthread_mutexattr_init(&attr) == 0);
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL) == 0);
	CHECK(pthread_mutex_init(mu, &attr) == 0);
	CHECK(pthread_mutexattr_destroy(&attr) == 0);
}

static void bad_real_recursive_mutex(void)
{
	CHECK(__cleanup_lockdep_mutex_lock(&real_mutex,
						  LOGGER_LOCK_INSTANCE_EMIT) == 0);
	(void)__cleanup_lockdep_mutex_lock(&real_mutex,
					  LOGGER_LOCK_INSTANCE_EMIT);
}

static void bad_real_non_lifo_unlock(void)
{
	CHECK(__cleanup_lockdep_mutex_lock(&real_control_mutex,
						  LOGGER_LOCK_GLOBAL_CONTROL) == 0);
	CHECK(__cleanup_lockdep_mutex_lock(&real_lifetime_mutex,
						  LOGGER_LOCK_GLOBAL_LIFETIME) == 0);
	atomic_store_explicit(unlock_calls, 0, memory_order_relaxed);
	unlock_probe_target = &real_control_mutex;
	unlock_probe_armed = 1;
	(void)__cleanup_lockdep_mutex_unlock(&real_control_mutex,
						     LOGGER_LOCK_GLOBAL_CONTROL);
}

static void *real_abba_side(void *arg)
{
	int left = arg == NULL;
	pthread_mutex_t *first = left ? &real_abba_a : &real_abba_b;
	pthread_mutex_t *second = left ? &real_abba_b : &real_abba_a;
	logger_lock_class_t first_class =
		left ? LOGGER_LOCK_INSTANCE_EMIT : LOGGER_LOCK_INSTANCE_PROGRESS;
	logger_lock_class_t second_class =
		left ? LOGGER_LOCK_INSTANCE_PROGRESS : LOGGER_LOCK_INSTANCE_EMIT;
	int rc = __cleanup_lockdep_mutex_lock(first, first_class);
	if (rc)
		return (void *)(intptr_t)rc;
	rc = pthread_barrier_wait(&real_abba_ready);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
		return (void *)(intptr_t)rc;
	rc = __cleanup_lockdep_mutex_lock(second, second_class);
	return rc ? (void *)(intptr_t)rc : NULL;
}

static void bad_real_reverse_lock_order(void)
{
	pthread_t left, right;
	CHECK(pthread_barrier_init(&real_abba_ready, NULL, 2) == 0);
	CHECK(pthread_create(&left, NULL, real_abba_side, NULL) == 0);
	CHECK(pthread_create(&right, NULL, real_abba_side, &real_abba_b) == 0);
	CHECK(pthread_join(left, NULL) == 0);
	CHECK(pthread_join(right, NULL) == 0);
}

static int barrier_wait_ok(pthread_barrier_t *barrier)
{
	int rc = pthread_barrier_wait(barrier);
	return rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD;
}

static void *hold_progress_for_trylock(void *arg)
{
	(void)arg;
	int rc = __cleanup_lockdep_mutex_lock(&real_trylock_mutex,
						     LOGGER_LOCK_INSTANCE_PROGRESS);
	if (rc)
		return (void *)(intptr_t)rc;
	if (!barrier_wait_ok(&real_trylock_ready) ||
	    !barrier_wait_ok(&real_trylock_done))
		return (void *)(intptr_t)EINVAL;
	rc = __cleanup_lockdep_mutex_unlock(&real_trylock_mutex,
						    LOGGER_LOCK_INSTANCE_PROGRESS);
	return rc ? (void *)(intptr_t)rc : NULL;
}

static void test_failed_mutex_trylock(void)
{
	pthread_t owner;
	void *thread_result;
	CHECK(pthread_barrier_init(&real_trylock_ready, NULL, 2) == 0);
	CHECK(pthread_barrier_init(&real_trylock_done, NULL, 2) == 0);
	CHECK(pthread_create(&owner, NULL, hold_progress_for_trylock, NULL) == 0);
	CHECK(barrier_wait_ok(&real_trylock_ready));
	CHECK(__cleanup_lockdep_mutex_lock(&real_abba_a,
						  LOGGER_LOCK_INSTANCE_EMIT) == 0);
	int rc = __cleanup_lockdep_mutex_trylock(&real_trylock_mutex,
							LOGGER_LOCK_INSTANCE_PROGRESS);
	CHECK(rc == EBUSY);
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &real_abba_a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_PROGRESS,
					       &real_trylock_mutex);
	CHECK(__cleanup_lockdep_mutex_unlock(&real_abba_a,
						     LOGGER_LOCK_INSTANCE_EMIT) == 0);
	CHECK(barrier_wait_ok(&real_trylock_done));
	CHECK(pthread_join(owner, &thread_result) == 0);
	CHECK(thread_result == NULL);
	CHECK(pthread_barrier_destroy(&real_trylock_ready) == 0);
	CHECK(pthread_barrier_destroy(&real_trylock_done) == 0);
}

static void test_successful_trylocks(void)
{
	CHECK(__cleanup_lockdep_mutex_trylock(&real_mutex,
						     LOGGER_LOCK_INSTANCE_EMIT) == 0);
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &real_mutex);
	CHECK(__cleanup_lockdep_mutex_unlock(&real_mutex,
						    LOGGER_LOCK_INSTANCE_EMIT) == 0);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_EMIT, &real_mutex);

	CHECK(pthread_rwlock_init(&real_rwlock, NULL) == 0);
	CHECK(__cleanup_lockdep_rwlock_trywrlock(&real_rwlock) == 0);
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(__cleanup_lockdep_rwlock_unlock(&real_rwlock) == 0);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(pthread_rwlock_destroy(&real_rwlock) == 0);
}

static void bad_successful_mutex_trylock_order(void)
{
	CHECK(__cleanup_lockdep_mutex_lock(&real_abba_a,
						  LOGGER_LOCK_INSTANCE_EMIT) == 0);
	(void)__cleanup_lockdep_mutex_trylock(&real_abba_b,
						     LOGGER_LOCK_INSTANCE_PROGRESS);
}

static void bad_successful_rwlock_trywrite_order(void)
{
	CHECK(pthread_rwlock_init(&real_rwlock, NULL) == 0);
	CHECK(__cleanup_lockdep_mutex_lock(&real_abba_a,
						  LOGGER_LOCK_INSTANCE_EMIT) == 0);
	(void)__cleanup_lockdep_rwlock_trywrlock(&real_rwlock);
}

static void test_rwlock_transitions(void)
{
	CHECK(pthread_rwlock_init(&real_rwlock, NULL) == 0);
	CHECK(__cleanup_lockdep_rwlock_rdlock(&real_rwlock) == 0);
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(__cleanup_lockdep_rwlock_unlock(&real_rwlock) == 0);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(__cleanup_lockdep_rwlock_wrlock(&real_rwlock) == 0);
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(__cleanup_lockdep_rwlock_unlock(&real_rwlock) == 0);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_LIFETIME, &real_rwlock);
	CHECK(pthread_rwlock_destroy(&real_rwlock) == 0);
}

static void *hold_rwlock_for_trywrite(void *arg)
{
	(void)arg;
	int rc = __cleanup_lockdep_rwlock_rdlock(&real_rwlock);
	if (rc)
		return (void *)(intptr_t)rc;
	if (!barrier_wait_ok(&real_rwlock_ready) ||
	    !barrier_wait_ok(&real_rwlock_done))
		return (void *)(intptr_t)EINVAL;
	rc = __cleanup_lockdep_rwlock_unlock(&real_rwlock);
	return rc ? (void *)(intptr_t)rc : NULL;
}

static void test_failed_rwlock_trywrite(void)
{
	pthread_t reader;
	void *thread_result;
	CHECK(pthread_rwlock_init(&real_rwlock, NULL) == 0);
	CHECK(pthread_barrier_init(&real_rwlock_ready, NULL, 2) == 0);
	CHECK(pthread_barrier_init(&real_rwlock_done, NULL, 2) == 0);
	CHECK(pthread_create(&reader, NULL, hold_rwlock_for_trywrite, NULL) == 0);
	CHECK(barrier_wait_ok(&real_rwlock_ready));
	CHECK(__cleanup_lockdep_mutex_lock(&real_abba_a,
						  LOGGER_LOCK_INSTANCE_EMIT) == 0);
	int rc = __cleanup_lockdep_rwlock_trywrlock(&real_rwlock);
	CHECK(rc == EBUSY || rc == EAGAIN);
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_EMIT, &real_abba_a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_LIFETIME,
					       &real_rwlock);
	CHECK(__cleanup_lockdep_mutex_unlock(&real_abba_a,
						     LOGGER_LOCK_INSTANCE_EMIT) == 0);
	CHECK(barrier_wait_ok(&real_rwlock_done));
	CHECK(pthread_join(reader, &thread_result) == 0);
	CHECK(thread_result == NULL);
	CHECK(pthread_barrier_destroy(&real_rwlock_ready) == 0);
	CHECK(pthread_barrier_destroy(&real_rwlock_done) == 0);
	CHECK(pthread_rwlock_destroy(&real_rwlock) == 0);
}

typedef struct {
	pthread_mutex_t *mu;
	pthread_mutex_t *other;
} tls_lock_arg_t;

static void *tls_lock_side(void *arg)
{
	tls_lock_arg_t *locks = arg;
	int rc = __cleanup_lockdep_mutex_lock(locks->mu,
						     LOGGER_LOCK_INSTANCE_PROGRESS);
	if (rc)
		return (void *)(intptr_t)rc;
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_PROGRESS, locks->mu);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_PROGRESS,
					       locks->other);
	if (!barrier_wait_ok(&tls_ready) || !barrier_wait_ok(&tls_done))
		return (void *)(intptr_t)EINVAL;
	rc = __cleanup_lockdep_mutex_unlock(locks->mu,
						   LOGGER_LOCK_INSTANCE_PROGRESS);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_PROGRESS, locks->mu);
	return rc ? (void *)(intptr_t)rc : NULL;
}

static void test_thread_local_tracking(void)
{
	pthread_t left, right;
	void *thread_result;
	tls_lock_arg_t left_locks = { &tls_mutex_a, &tls_mutex_b };
	tls_lock_arg_t right_locks = { &tls_mutex_b, &tls_mutex_a };
	CHECK(pthread_barrier_init(&tls_ready, NULL, 2) == 0);
	CHECK(pthread_barrier_init(&tls_done, NULL, 2) == 0);
	CHECK(pthread_create(&left, NULL, tls_lock_side, &left_locks) == 0);
	CHECK(pthread_create(&right, NULL, tls_lock_side, &right_locks) == 0);
	CHECK(pthread_join(left, &thread_result) == 0);
	CHECK(thread_result == NULL);
	CHECK(pthread_join(right, &thread_result) == 0);
	CHECK(thread_result == NULL);
	CHECK(pthread_barrier_destroy(&tls_ready) == 0);
	CHECK(pthread_barrier_destroy(&tls_done) == 0);
}

static void *signal_cond_waiter(void *arg)
{
	(void)arg;
	int rc = __cleanup_lockdep_mutex_lock(&cond_mutex,
						     LOGGER_LOCK_INSTANCE_PROGRESS);
	if (rc)
		return (void *)(intptr_t)rc;
	cond_ready = 1;
	rc = pthread_cond_signal(&cond_cv);
	int unlock_rc = __cleanup_lockdep_mutex_unlock(
		&cond_mutex, LOGGER_LOCK_INSTANCE_PROGRESS);
	if (rc)
		return (void *)(intptr_t)rc;
	return unlock_rc ? (void *)(intptr_t)unlock_rc : NULL;
}

static void test_cond_wait_reacquire_tracking(void)
{
	pthread_t signaler;
	void *thread_result;
	cond_ready = 0;
	CHECK(pthread_mutex_init(&cond_mutex, NULL) == 0);
	CHECK(pthread_cond_init(&cond_cv, NULL) == 0);
	CHECK(__cleanup_lockdep_mutex_lock(&cond_mutex,
						  LOGGER_LOCK_INSTANCE_PROGRESS) == 0);
	CHECK(pthread_create(&signaler, NULL, signal_cond_waiter, NULL) == 0);
	while (!cond_ready)
		CHECK(pthread_cond_wait(&cond_cv, &cond_mutex) == 0);
	logger_lockdep_assert_held(LOGGER_LOCK_INSTANCE_PROGRESS, &cond_mutex);
	CHECK(__cleanup_lockdep_mutex_unlock(&cond_mutex,
						     LOGGER_LOCK_INSTANCE_PROGRESS) == 0);
	logger_lockdep_assert_not_held(LOGGER_LOCK_INSTANCE_PROGRESS, &cond_mutex);
	CHECK(pthread_join(signaler, &thread_result) == 0);
	CHECK(pthread_cond_destroy(&cond_cv) == 0);
	CHECK(pthread_mutex_destroy(&cond_mutex) == 0);
	CHECK(thread_result == NULL);
}

int main(int argc, char **argv)
{
	unlock_calls = mmap(NULL, sizeof(*unlock_calls), PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(unlock_calls != MAP_FAILED);
	atomic_init(unlock_calls, 0);
	init_normal_mutex(&real_mutex);
	init_normal_mutex(&real_control_mutex);
	init_normal_mutex(&real_lifetime_mutex);
	init_normal_mutex(&real_abba_a);
	init_normal_mutex(&real_abba_b);
	init_normal_mutex(&real_trylock_mutex);
	init_normal_mutex(&tls_mutex_a);
	init_normal_mutex(&tls_mutex_b);
	if (argc == 2) {
		if (!strcmp(argv[1], "real-recursive"))
			expect_abort(bad_real_recursive_mutex);
		else if (!strcmp(argv[1], "real-abba"))
			expect_abort(bad_real_reverse_lock_order);
		else if (!strcmp(argv[1], "real-unlock")) {
			expect_abort(bad_real_non_lifo_unlock);
			CHECK(atomic_load_explicit(unlock_calls,
						   memory_order_relaxed) == 0);
		} else if (!strcmp(argv[1], "mutex-trylock")) {
			test_failed_mutex_trylock();
		} else if (!strcmp(argv[1], "trylock-success")) {
			test_successful_trylocks();
		} else if (!strcmp(argv[1], "trylock-success-order")) {
			expect_abort(bad_successful_mutex_trylock_order);
		} else if (!strcmp(argv[1], "trywrlock-success-order")) {
			expect_abort(bad_successful_rwlock_trywrite_order);
		} else if (!strcmp(argv[1], "rwlock")) {
			test_rwlock_transitions();
		} else if (!strcmp(argv[1], "rwlock-trywrite")) {
			test_failed_rwlock_trywrite();
		} else if (!strcmp(argv[1], "thread-local")) {
			test_thread_local_tracking();
		} else if (!strcmp(argv[1], "cond-wait")) {
			test_cond_wait_reacquire_tracking();
		} else {
			return 2;
		}
		return 0;
	}
	CHECK(argc == 1);

	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_assert_held(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_LIFETIME, &b);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_assert_not_held(LOGGER_LOCK_GLOBAL_CONTROL, &a);

	/* Controller 持有的候选/退役对象：生命周期尚未发布。 */
	logger_lockdep_acquire(LOGGER_LOCK_GLOBAL_CONTROL, &a);
	logger_lockdep_acquire(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_INSTANCE_EMIT, &c);
	logger_lockdep_release(LOGGER_LOCK_GLOBAL_CONTROL, &a);

	expect_abort(bad_instance_nesting);
	expect_abort(bad_reverse_global);
	expect_abort(bad_release);
	expect_abort(bad_assert);

	preflight_checks_do_not_change_state();
	expect_abort(bad_preflight_recursive_acquire);
	expect_abort(bad_preflight_lock_order);
	expect_abort(bad_preflight_release_order);

	expect_abort(bad_real_recursive_mutex);
	expect_abort(bad_real_reverse_lock_order);
	expect_abort(bad_real_non_lifo_unlock);
	expect_abort(bad_successful_mutex_trylock_order);
	expect_abort(bad_successful_rwlock_trywrite_order);
	CHECK(atomic_load_explicit(unlock_calls, memory_order_relaxed) == 0);
	test_failed_mutex_trylock();
	test_successful_trylocks();
	test_rwlock_transitions();
	test_failed_rwlock_trywrite();
	test_thread_local_tracking();
	test_cond_wait_reacquire_tracking();

	return 0;
}
