#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"

static logger_t *owner;
static int active;
static int fail_setup;
static int fail_restore;
static int fail_read;
static int fail_release;
static unsigned int cancel_calls;
static unsigned int read_calls;
static unsigned int release_calls;

int __real_pthread_setcancelstate(int state, int *old);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *rw);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *rw);

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (active) {
		++cancel_calls;
		if (state == PTHREAD_CANCEL_DISABLE && fail_setup) {
			int error = fail_setup;

			fail_setup = 0;
			errno = ERANGE;
			return error;
		}
	}
	int rc = __real_pthread_setcancelstate(state, old);
	/* 先实际恢复调用线程的取消策略，再模拟恢复错误。 */
	if (active && !rc && state == PTHREAD_CANCEL_ENABLE && fail_restore) {
		int error = fail_restore;

		fail_restore = 0;
		errno = ERANGE;
		return error;
	}
	return rc;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
	if (active && rw == &logger_runtime_current()->global_lifetime_lock) {
		++read_calls;
		if (fail_read) {
			int error = fail_read;

			fail_read = 0;
			errno = ERANGE;
			return error;
		}
	}
	return __real_pthread_rwlock_rdlock(rw);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
	int rc = __real_pthread_rwlock_unlock(rw);

	if (active && rw == &logger_runtime_current()->global_lifetime_lock) {
		++release_calls;
		if (!rc && fail_release) {
			int error = fail_release;

			fail_release = 0;
			errno = ERANGE;
			return error;
		}
	}
	return rc;
}

static void expect_level(logger_level_t level)
{
	logger_diagnostics_t diag;

	errno = EDOM;
	CHECK(logger_get_diagnostics(owner, &diag) == 0 && errno == EDOM);
	CHECK(diag.level == level);
}

static void expect_default_level(logger_level_t level)
{
	logger_diagnostics_t diag;

	errno = EDOM;
	CHECK(logger_get_global_diagnostics(&diag) == 0 && errno == EDOM);
	CHECK(diag.level == level);
}

static void *concurrent_setter(void *data)
{
	logger_level_t level = *(logger_level_t *)data;

	for (unsigned int i = 0; i < 1000; ++i) {
		errno = EDOM;
		CHECK(logger_set_instance_level(owner, level) == 0);
		CHECK(errno == EDOM);
	}
	return NULL;
}

static void start_setter(pthread_t *thread, logger_level_t *level)
{
	int rc = pthread_create(thread, NULL, concurrent_setter, level);

	CHECK(rc == 0);
}

static void check_instance_range(void)
{
	for (int level = LOGGER_TRACE; level <= LOGGER_OFF; ++level) {
		errno = EDOM;
		logger_level_t value = (logger_level_t)level;

		CHECK(logger_set_instance_level(owner, value) == 0);
		CHECK(errno == EDOM);
		expect_level((logger_level_t)level);
	}
	CHECK(logger_set_instance_level(owner, LOGGER_INFO) == 0);
	errno = EDOM;
	logger_level_t invalid = (logger_level_t)-1;

	CHECK(logger_set_instance_level(owner, invalid) == -EINVAL);
	CHECK(errno == EDOM);
	invalid = (logger_level_t)(LOGGER_OFF + 1);
	CHECK(logger_set_instance_level(owner, invalid) == -EINVAL);
	CHECK(errno == EDOM);
	CHECK(logger_set_instance_level(NULL, LOGGER_INFO) == -EINVAL);
	CHECK(errno == EDOM);
	expect_level(LOGGER_INFO);
}

static void check_stopped_instance(void)
{
	for (logger_state_t state = LOGGER_STATE_STOPPING;
	     state <= LOGGER_STATE_STOPPED; ++state) {
		atomic_store_explicit(&owner->state, state,
				      memory_order_release);
		errno = EDOM;
		CHECK(logger_set_instance_level(owner, LOGGER_WARN) == 0);
		CHECK(errno == EDOM);
		expect_level(LOGGER_WARN);
	}
	atomic_store_explicit(&owner->state, LOGGER_STATE_RUNNING,
			      memory_order_release);
	CHECK(logger_set_instance_level(owner, LOGGER_INFO) == 0);
}

static void check_global_invalid(void)
{
	errno = EDOM;
	CHECK(logger_set_level((logger_level_t)-1) == -EINVAL);
	CHECK(errno == EDOM);
	CHECK(logger_set_level((logger_level_t)(LOGGER_OFF + 1)) == -EINVAL);
	CHECK(errno == EDOM);
	expect_default_level(LOGGER_INFO);
}

static void check_global_ready(void)
{
	for (int level = LOGGER_TRACE; level <= LOGGER_OFF; ++level) {
		errno = EDOM;
		CHECK(logger_set_level((logger_level_t)level) == 0);
		CHECK(errno == EDOM);
		expect_default_level((logger_level_t)level);
	}
	CHECK(logger_set_level(LOGGER_INFO) == 0);
	check_global_invalid();
}

static void check_fault(const char *scenario)
{
	int expected = 0;
	logger_level_t requested = LOGGER_WARN;

	if (!strcmp(scenario, "setup")) {
		expected = EAGAIN;
		fail_setup = expected;
	} else if (!strcmp(scenario, "restore")) {
		expected = EIO;
		fail_restore = expected;
	} else if (!strcmp(scenario, "read-lock")) {
		expected = EBUSY;
		fail_read = expected;
	} else if (!strcmp(scenario, "read-unlock")) {
		expected = EPROTO;
		fail_release = expected;
	} else if (!strcmp(scenario, "invalid-restore")) {
		fail_restore = EIO;
		expected = EINVAL;
		requested = (logger_level_t)-1;
	} else {
		CHECK(!"unknown fault scenario");
	}
	errno = EDOM;
	active = 1;
	int rc = logger_set_level(requested);
	int saved = errno;

	active = 0;
	CHECK(rc == -expected && saved == EDOM);
	CHECK(!fail_setup && !fail_restore && !fail_read && !fail_release);
	if (!strcmp(scenario, "setup")) {
		CHECK(cancel_calls == 1 && read_calls == 0);
		expect_default_level(LOGGER_INFO);
	} else if (!strcmp(scenario, "restore")) {
		CHECK(cancel_calls == 2 && read_calls == 1);
		CHECK(release_calls == 1);
		expect_default_level(LOGGER_WARN);
	} else if (!strcmp(scenario, "read-lock")) {
		CHECK(cancel_calls == 2 && read_calls == 1 && !release_calls);
		expect_default_level(LOGGER_INFO);
	} else if (!strcmp(scenario, "invalid-restore")) {
		CHECK(cancel_calls == 2 && !read_calls && !release_calls);
		expect_default_level(LOGGER_INFO);
	} else {
		/* 解锁证明失败时不得归还默认借用或强制释放宿主对象。 */
		CHECK(read_calls == 1 && release_calls == 1);
		CHECK(atomic_load(&owner->level) == LOGGER_WARN);
		CHECK(atomic_load(&owner->default_borrowed) == 1);
		struct logger_runtime *runtime = logger_runtime_current();

		CHECK(atomic_load(&runtime->global_lock_error) == EPROTO);
		CHECK(logger_set_level(LOGGER_INFO) == -EDEADLK);
		return;
	}
	CHECK(logger_set_level(LOGGER_INFO) == 0);
	expect_default_level(LOGGER_INFO);
}

static void check_cancel_disabled(void)
{
	int old_state;
	int old_type;
	int observed;

	CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state) == 0);
	CHECK(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type) ==
	      0);
	errno = EDOM;
	active = 1;
	CHECK(logger_set_level(LOGGER_DEBUG) == 0 && errno == EDOM);
	CHECK(logger_set_instance_level(owner, LOGGER_WARN) == 0 &&
	      errno == EDOM);
	active = 0;
	CHECK(cancel_calls == 2 && read_calls == 1 && release_calls == 1);
	CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &observed) == 0);
	CHECK(observed == PTHREAD_CANCEL_DISABLE);
	int type = PTHREAD_CANCEL_ASYNCHRONOUS;

	CHECK(pthread_setcanceltype(type, &observed) == 0);
	CHECK(observed == PTHREAD_CANCEL_ASYNCHRONOUS);
	CHECK(pthread_setcanceltype(old_type, NULL) == 0);
	CHECK(pthread_setcancelstate(old_state, NULL) == 0);
	expect_default_level(LOGGER_WARN);
	CHECK(logger_set_level(LOGGER_INFO) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *scenario = argv[1];

	if (!strcmp(scenario, "cold")) {
		errno = EDOM;
		CHECK(logger_set_instance_level((logger_t *)(uintptr_t)1,
						LOGGER_INFO) == -ENODEV);
		CHECK(errno == EDOM);
		CHECK(logger_set_level(LOGGER_INFO) == -ENODEV);
		CHECK(errno == EDOM);
		CHECK(logger_set_level((logger_level_t)-1) == -ENODEV);
		CHECK(errno == EDOM);
	} else {
		CHECK(logger_process_init() == 0);
		if (!strcmp(scenario, "unbound")) {
			errno = EDOM;
			CHECK(logger_set_level(LOGGER_WARN) == -ENODEV);
			CHECK(errno == EDOM);
			CHECK(logger_set_level((logger_level_t)-1) == -EINVAL);
			CHECK(errno == EDOM);
		} else {
			logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

			cfg.async_mode = 0;
			CHECK(logger_create(&cfg, &owner) == 0 && owner);
			int bound = strcmp(scenario, "instance") &&
				    strcmp(scenario, "stopped") &&
				    strcmp(scenario, "concurrent");
			if (bound)
				CHECK(logger_default_set(NULL, owner) == 0);
			if (!strcmp(scenario, "instance")) {
				check_instance_range();
			} else if (!strcmp(scenario, "stopped")) {
				check_stopped_instance();
			} else if (!strcmp(scenario, "concurrent")) {
				logger_level_t levels[2];

				levels[0] = LOGGER_DEBUG;
				levels[1] = LOGGER_WARN;
				pthread_t threads[2];

				for (int i = 0; i < 2; ++i)
					start_setter(&threads[i], &levels[i]);
				for (int i = 0; i < 2; ++i)
					CHECK(!pthread_join(threads[i], NULL));
				check_instance_range();
			} else if (!strcmp(scenario, "global")) {
				check_global_ready();
			} else if (!strcmp(scenario, "disabled")) {
				check_cancel_disabled();
			} else {
				check_fault(scenario);
			}
			if (strcmp(scenario, "read-unlock") != 0) {
				if (bound)
					CHECK(!logger_default_set(owner, NULL));
				CHECK(logger_destroy(&owner) == 0);
				CHECK(owner == NULL);
				CHECK(logger_process_object_count() == 0);
			}
		}
	}
	printf("LEVEL_STATUS_PASS=%s\n", scenario);
	return 0;
}
