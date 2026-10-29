#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

enum query {
	Q_STATE,
	Q_DROPPED,
	Q_METRICS,
	Q_IO,
	Q_DIAGNOSTICS,
	Q_FILE,
	Q_SYSLOG,
	Q_DEFAULT_DROPPED,
	Q_DEFAULT_METRICS,
	Q_DEFAULT_IO,
	Q_DEFAULT_DIAGNOSTICS,
	Q_DEFAULT_FILE,
	Q_COUNT
};

union query_value {
	logger_state_t state;
	uint64_t dropped;
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_diagnostics_t diagnostics;
	logger_file_metrics_t file;
	logger_syslog_metrics_t syslog;
};

struct query_slot {
	unsigned char before[16];
	union query_value value;
	unsigned char after[16];
};

static logger_t *owner;
static logger_state_t expected_state = LOGGER_STATE_RUNNING;
static int active;
static int fail_setup;
static int fail_restore;
static int fail_emit_lock;
static int fail_emit_unlock;
static int fail_read_lock;
static int fail_read_unlock;
static unsigned int cancel_calls;
static unsigned int mutex_calls;
static unsigned int emit_unlock_calls;
static unsigned int read_calls;
static unsigned int read_unlock_calls;
static unsigned int io_calls;
static unsigned int checks;

int __real_pthread_setcancelstate(int state, int *old);
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int __real_pthread_rwlock_unlock(pthread_rwlock_t *lock);
ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __real_writev(int fd, const struct iovec *iov, int count);
int __real_fsync(int fd);
int __real_connect(int fd, const struct sockaddr *address, socklen_t size);
ssize_t __real_send(int fd, const void *data, size_t size, int flags);

static int take_error(int *slot)
{
	int error = *slot;

	*slot = 0;
	if (error)
		errno = ERANGE;
	return error;
}

int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (active) {
		++cancel_calls;
		if (state == PTHREAD_CANCEL_DISABLE && fail_setup)
			return take_error(&fail_setup);
	}
	int rc = __real_pthread_setcancelstate(state, old);

	/* 故障发生在真实恢复之后，错误码不借用测试设置的 errno。 */
	if (active && !rc && state == PTHREAD_CANCEL_ENABLE && fail_restore)
		return take_error(&fail_restore);
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (active) {
		++mutex_calls;
		if (mutex == &owner->emit_mu && fail_emit_lock)
			return take_error(&fail_emit_lock);
	}
	return __real_pthread_mutex_lock(mutex);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc = __real_pthread_mutex_unlock(mutex);

	if (active && mutex == &owner->emit_mu) {
		++emit_unlock_calls;
		if (!rc && fail_emit_unlock)
			return take_error(&fail_emit_unlock);
	}
	return rc;
}

int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	if (active && lock == &logger_runtime_current()->global_lifetime_lock) {
		++read_calls;
		if (fail_read_lock)
			return take_error(&fail_read_lock);
	}
	return __real_pthread_rwlock_rdlock(lock);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	int rc = __real_pthread_rwlock_unlock(lock);

	if (active && lock == &logger_runtime_current()->global_lifetime_lock) {
		++read_unlock_calls;
		if (!rc && fail_read_unlock)
			return take_error(&fail_read_unlock);
	}
	return rc;
}

ssize_t __wrap_write(int fd, const void *data, size_t size)
{
	if (active)
		++io_calls;
	return __real_write(fd, data, size);
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int count)
{
	if (active)
		++io_calls;
	return __real_writev(fd, iov, count);
}

int __wrap_fsync(int fd)
{
	if (active)
		++io_calls;
	return __real_fsync(fd);
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t size)
{
	if (active)
		++io_calls;
	return __real_connect(fd, address, size);
}

ssize_t __wrap_send(int fd, const void *data, size_t size, int flags)
{
	if (active)
		++io_calls;
	return __real_send(fd, data, size, flags);
}

static void reset_probe(void)
{
	CHECK(!active);
	fail_setup = 0;
	fail_restore = 0;
	fail_emit_lock = 0;
	fail_emit_unlock = 0;
	fail_read_lock = 0;
	fail_read_unlock = 0;
	cancel_calls = 0;
	mutex_calls = 0;
	emit_unlock_calls = 0;
	read_calls = 0;
	read_unlock_calls = 0;
	io_calls = 0;
}

static int query(enum query op, union query_value *out)
{
	switch (op) {
	case Q_STATE:
		return logger_get_state(owner, out ? &out->state : NULL);
	case Q_DROPPED:
		return logger_dropped(owner, out ? &out->dropped : NULL);
	case Q_METRICS:
		return logger_get_metrics(owner, out ? &out->metrics : NULL);
	case Q_IO:
		return logger_get_io_metrics(owner, out ? &out->io : NULL);
	case Q_DIAGNOSTICS:
		return logger_get_diagnostics(owner,
					      out ? &out->diagnostics : NULL);
	case Q_FILE:
		return logger_get_file_metrics(owner, out ? &out->file : NULL);
	case Q_SYSLOG:
		return logger_get_syslog_metrics(owner,
						 out ? &out->syslog : NULL);
	case Q_DEFAULT_DROPPED:
		return logger_global_dropped(out ? &out->dropped : NULL);
	case Q_DEFAULT_METRICS:
		return logger_get_global_metrics(out ? &out->metrics : NULL);
	case Q_DEFAULT_IO:
		return logger_get_global_io_metrics(out ? &out->io : NULL);
	case Q_DEFAULT_DIAGNOSTICS:
		return logger_get_global_diagnostics(out ? &out->diagnostics :
							   NULL);
	case Q_DEFAULT_FILE:
		return logger_get_global_file_metrics(out ? &out->file : NULL);
	default:
		CHECK(0);
		return -EINVAL;
	}
}

static void check_value(enum query op, const union query_value *out)
{
	switch (op) {
	case Q_STATE:
		CHECK(out->state == expected_state);
		break;
	case Q_DROPPED:
	case Q_DEFAULT_DROPPED:
		CHECK(out->dropped == 0);
		break;
	case Q_METRICS:
	case Q_DEFAULT_METRICS:
		CHECK(out->metrics.enqueued == 0);
		CHECK(out->metrics.sync_fallbacks == 0);
		for (size_t i = 0; i < 6; ++i)
			CHECK(out->metrics.dropped[i] == 0);
		break;
	case Q_IO:
	case Q_DEFAULT_IO:
		CHECK(out->io.emitted_records == 1);
		CHECK(out->io.sync_completed == 1 &&
		      out->io.failed_records == 0);
		break;
	case Q_DIAGNOSTICS:
	case Q_DEFAULT_DIAGNOSTICS:
		CHECK(out->diagnostics.state == expected_state);
		CHECK(out->diagnostics.emitted_records == 1);
		CHECK(out->diagnostics.async_mode == 0);
		break;
	case Q_FILE:
	case Q_DEFAULT_FILE:
		CHECK(out->file.write_operations == 1);
		CHECK(out->file.bytes_written > 0);
		CHECK(out->file.current_size == out->file.bytes_written);
		break;
	case Q_SYSLOG:
		CHECK(out->syslog.connected == 1);
		CHECK(out->syslog.sent_records == 1);
		CHECK(out->syslog.connect_attempts == 1);
		break;
	default:
		CHECK(0);
	}
}

static void exercise(enum query op, int error)
{
	struct query_slot slot;
	unsigned char before[sizeof(slot)];

	memset(&slot, 0xa5, sizeof(slot));
	memcpy(before, &slot, sizeof(before));
	errno = EDOM;
	active = 1;
	int rc = query(op, &slot.value);
	int saved = errno;

	active = 0;
	CHECK(rc == -error && saved == EDOM);
	CHECK(io_calls == 0);
	if (error) {
		CHECK(!memcmp(before, &slot, sizeof(slot)));
	} else {
		for (size_t i = 0; i < sizeof(slot.before); ++i)
			CHECK(slot.before[i] == 0xa5 && slot.after[i] == 0xa5);
		check_value(op, &slot.value);
	}
	++checks;
}

static const enum query scoped_queries[] = {
	Q_FILE,	      Q_DEFAULT_DROPPED,     Q_DEFAULT_METRICS,
	Q_DEFAULT_IO, Q_DEFAULT_DIAGNOSTICS, Q_DEFAULT_FILE
};

static void healthy_again(enum query op)
{
	CHECK(!logger_scope_busy() && !logger_global_scope_busy());
	reset_probe();
	exercise(op, 0);
	CHECK(!logger_scope_busy() && !logger_global_scope_busy());
}

static void cancellation_matrix(int setup)
{
	for (size_t i = 0;
	     i < sizeof(scoped_queries) / sizeof(scoped_queries[0]); ++i) {
		enum query op = scoped_queries[i];

		reset_probe();
		if (setup)
			fail_setup = EAGAIN;
		else
			fail_restore = EIO;
		exercise(op, setup ? EAGAIN : EIO);
		CHECK(!fail_setup && !fail_restore);
		CHECK(cancel_calls == (setup ? 1u : 2u));
		if (setup)
			CHECK(!mutex_calls && !read_calls);
		else {
			CHECK(mutex_calls ==
			      ((op == Q_FILE || op == Q_DEFAULT_FILE) ? 1u :
									0u));
			CHECK(read_calls == (op == Q_FILE ? 0u : 1u));
		}
		healthy_again(op);
	}
	/* 原先禁用取消的调用者继续保持禁用，类型也不能被查询修改。 */
	int old_state;
	int old_type;

	CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state) == 0);
	CHECK(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type) ==
	      0);
	for (size_t i = 0;
	     i < sizeof(scoped_queries) / sizeof(scoped_queries[0]); ++i) {
		reset_probe();
		exercise(scoped_queries[i], 0);
		int observed;

		CHECK(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					     &observed) == 0);
		CHECK(observed == PTHREAD_CANCEL_DISABLE);
		CHECK(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					    &observed) == 0);
		CHECK(observed == PTHREAD_CANCEL_ASYNCHRONOUS);
	}
	CHECK(pthread_setcanceltype(old_type, NULL) == 0);
	CHECK(pthread_setcancelstate(old_state, NULL) == 0);
}

static void no_scope(void)
{
	for (enum query op = Q_STATE; op <= Q_DIAGNOSTICS; ++op) {
		reset_probe();
		fail_setup = EAGAIN;
		fail_emit_lock = EBUSY;
		fail_read_lock = EACCES;
		exercise(op, 0);
		CHECK(!cancel_calls && !mutex_calls && !read_calls);
		CHECK(fail_setup == EAGAIN && fail_emit_lock == EBUSY &&
		      fail_read_lock == EACCES);
	}
	reset_probe();
}

static void read_lock_matrix(void)
{
	for (enum query op = Q_DEFAULT_DROPPED; op < Q_COUNT; ++op) {
		reset_probe();
		fail_read_lock = EBUSY;
		exercise(op, EBUSY);
		CHECK(!fail_read_lock && read_calls == 1 && !read_unlock_calls);
		CHECK(!mutex_calls && cancel_calls == 2);
		healthy_again(op);
	}
}

static void emit_lock(enum query op, int restore)
{
	reset_probe();
	fail_emit_lock = ENOSPC;
	if (restore)
		fail_restore = EIO;
	exercise(op, ENOSPC);
	CHECK(!fail_emit_lock && !fail_restore);
	CHECK(mutex_calls == 1 && !emit_unlock_calls);
	healthy_again(op);
}

static void primary_errors(void)
{
	emit_lock(Q_FILE, 1);
	emit_lock(Q_DEFAULT_FILE, 1);
	for (size_t i = 0;
	     i < sizeof(scoped_queries) / sizeof(scoped_queries[0]); ++i) {
		reset_probe();
		fail_restore = EIO;
		errno = EDOM;
		active = 1;
		int rc = query(scoped_queries[i], NULL);
		int saved = errno;

		active = 0;
		CHECK(rc == -EINVAL && saved == EDOM);
		CHECK(!fail_restore && !mutex_calls && !read_calls &&
		      !io_calls);
		healthy_again(scoped_queries[i]);
	}
	reset_probe();
	fail_restore = EIO;
	exercise(Q_SYSLOG, ENOTSUP);
	CHECK(!fail_restore && !mutex_calls);
}

static void emit_unlock(enum query op)
{
	reset_probe();
	fail_emit_unlock = EIO;
	exercise(op, EIO);
	CHECK(!fail_emit_unlock && mutex_calls == 1 && emit_unlock_calls == 1);
	CHECK(atomic_load(&owner->synchronization_error) == EIO);
	CHECK(atomic_load(&owner->lifecycle_error) == EIO);
	expected_state = LOGGER_STATE_STOPPING;
	reset_probe();
	exercise(op, EIO);
	CHECK(!mutex_calls && !emit_unlock_calls);
	/* 同步证明失败的存活对象仍能诊断，不能新增 RUNNING 门槛。 */
	for (enum query plain = Q_STATE; plain <= Q_DIAGNOSTICS; ++plain) {
		reset_probe();
		exercise(plain, 0);
		CHECK(!cancel_calls && !mutex_calls && !read_calls);
	}
	for (enum query plain = Q_DEFAULT_DROPPED; plain < Q_DEFAULT_FILE;
	     ++plain) {
		reset_probe();
		exercise(plain, 0);
	}
	reset_probe();
	exercise(Q_DEFAULT_FILE, EIO);
	CHECK(!mutex_calls);
}

static void *poisoned_reader(void *arg)
{
	enum query op = *(enum query *)arg;

	exercise(op, EPROTO);
	return NULL;
}

static void read_unlock(enum query op, int primary)
{
	reset_probe();
	fail_read_unlock = EPROTO;
	if (primary) {
		fail_emit_lock = ENOSPC;
		fail_restore = EIO;
	}
	exercise(op, EPROTO);
	CHECK(!fail_read_unlock && read_calls == 1 && read_unlock_calls == 1);
	CHECK(atomic_load(&logger_runtime_current()->global_lock_error) ==
	      EPROTO);
	CHECK(atomic_load(&owner->default_borrowed) == 1);
	if (primary) {
		/* 借用释放证明失败比内层操作错误优先，并禁止继续恢复取消。 */
		CHECK(!fail_emit_lock && fail_restore == EIO &&
		      cancel_calls == 1);
	}
	reset_probe();
	/* 原调用线程保留重入保护；独立线程必须在重新加锁之前看到毒化。 */
	pthread_t thread;

	CHECK(pthread_create(&thread, NULL, poisoned_reader, &op) == 0);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(!read_calls && !mutex_calls && !read_unlock_calls);
	CHECK(atomic_load(&owner->default_borrowed) == 1);
}

static int receiver(void)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };

	CHECK(fd >= 0);
	memcpy(address.sun_path, "syslog.sock", sizeof("syslog.sock"));
	CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *mode = argv[1];
	char path[] = "/tmp/logger-query-error-XXXXXX";
	int syslog = !strncmp(mode, "syslog-", 7);
	int retained = 0;

	enter_temp(path);
	CHECK(logger_process_init() == 0);
	int fd = syslog ? receiver() : -1;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.outputs = LOGGER_OUT_FILE | (syslog ? LOGGER_OUT_SYSLOG : 0);
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = 0;
	cfg.syslog.path = "syslog.sock";
	CHECK(logger_create(&cfg, &owner) == 0);
	CHECK(logger_default_set(NULL, owner) == 0);
	CHECK(LOGGER_INFO(owner, "query", "before-error") == 0);
	if (!strcmp(mode, "setup") || !strcmp(mode, "restore")) {
		cancellation_matrix(!strcmp(mode, "setup"));
	} else if (!strcmp(mode, "no-scope")) {
		no_scope();
	} else if (!strcmp(mode, "read-lock")) {
		read_lock_matrix();
	} else if (!strcmp(mode, "emit-lock")) {
		emit_lock(Q_FILE, 0);
		emit_lock(Q_DEFAULT_FILE, 0);
	} else if (!strcmp(mode, "primary")) {
		primary_errors();
	} else if (!strcmp(mode, "emit-unlock") ||
		   !strcmp(mode, "syslog-unlock")) {
		emit_unlock(syslog ? Q_SYSLOG : Q_FILE);
		retained = 1;
	} else if (!strcmp(mode, "release-primary")) {
		read_unlock(Q_DEFAULT_FILE, 1);
		retained = 1;
	} else if (!strncmp(mode, "read-unlock-", 12)) {
		static const char *const names[] = { "dropped", "metrics", "io",
						     "diagnostics", "file" };
		unsigned int index;

		for (index = 0; index < sizeof(names) / sizeof(names[0]);
		     ++index)
			if (!strcmp(mode + 12, names[index]))
				break;
		CHECK(index < sizeof(names) / sizeof(names[0]));
		read_unlock(Q_DEFAULT_DROPPED + index, 0);
		retained = 1;
	} else if (!strcmp(mode, "syslog-restore")) {
		reset_probe();
		fail_setup = EAGAIN;
		exercise(Q_SYSLOG, EAGAIN);
		CHECK(!fail_setup && cancel_calls == 1 && mutex_calls == 0 &&
		      read_calls == 0 && io_calls == 0);
		healthy_again(Q_SYSLOG);
		reset_probe();
		fail_restore = EIO;
		exercise(Q_SYSLOG, EIO);
		CHECK(!fail_restore && cancel_calls == 2 && mutex_calls == 1);
		healthy_again(Q_SYSLOG);
	} else {
		CHECK(!strcmp(mode, "syslog-lock"));
		emit_lock(Q_SYSLOG, 1);
	}

	reset_probe();
	if (retained) {
		logger_t *before = owner;

		errno = EDOM;
		CHECK(logger_destroy(&owner) == -EBUSY && errno == EDOM);
		CHECK(owner == before &&
		      atomic_load(&owner->default_borrowed) == 1);
		/* 不伪造释放证明、重置毒化或删掉仍被持有的协调 inode。
		 * 这些失败用例由进程退出归还资源，不能记为健康销毁覆盖。
		 */
	} else {
		CHECK(!logger_scope_busy() && !logger_global_scope_busy());
		CHECK(logger_default_set(owner, NULL) == 0);
		CHECK(logger_destroy(&owner) == 0 && owner == NULL);
		CHECK(logger_process_object_count() == 0);
	}
	if (syslog) {
		CHECK(close(fd) == 0);
		CHECK(unlink("syslog.sock") == 0);
	}
	if (retained)
		CHECK(chdir("/") == 0);
	else
		leave_temp(path);
	printf("QUERY_ERROR_PASS=%s checks=%u retained=%d\n", mode, checks,
	       retained);
	return 0;
}
