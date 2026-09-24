#define _GNU_SOURCE
#include "host_support.h"
#include "logger_internal.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int data_open_error, rename_error, fixed_time, sync_error,
	close_error_fd = -1;
static int old_fd = -1, renamed, rename_calls, vector_max, unlink_error,
	   flock_eintr, new_synced;
static _Atomic int failures, wins;
static logger_t *winner;
static struct timespec frozen = { 1700000000, 0 };

int __real_openat(int, const char *, int, ...);
int __real_fsync(int);
int __real_close(int);
int __real_clock_gettime(clockid_t, struct timespec *);
int __real_logger_file_rename_noreplace(int, const char *, const char *);
int __real_unlinkat(int, const char *, int);
int __real_flock(int, int);
ssize_t __real_writev(int, const struct iovec *, int);

int __wrap_openat(int d, const char *p, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	if (data_open_error && !strcmp(p, "out.log")) {
		errno = data_open_error;
		return -1;
	}
	return __real_openat(d, p, flags, mode);
}
int __wrap_clock_gettime(clockid_t which, struct timespec *t)
{
	if (fixed_time && which == CLOCK_REALTIME) {
		*t = frozen;
		return 0;
	}
	return __real_clock_gettime(which, t);
}
int __wrap_logger_file_rename_noreplace(int d, const char *a, const char *b)
{
	++rename_calls;
	if (rename_error) {
		errno = rename_error;
		return -1;
	}
	int rc = __real_logger_file_rename_noreplace(d, a, b);
	if (!rc)
		renamed = 1;
	return rc;
}
int __wrap_fsync(int fd)
{
	struct stat st;
	CHECK(fstat(fd, &st) == 0);
	int match = (sync_error == 1 && fd == old_fd) ||
		    (sync_error == 2 && renamed && S_ISDIR(st.st_mode)) ||
		    (sync_error == 3 && renamed && S_ISREG(st.st_mode) &&
		     fd != old_fd) ||
		    (sync_error == 5 && new_synced && S_ISDIR(st.st_mode)) ||
		    (sync_error == 6 && S_ISDIR(st.st_mode));
	if (match) {
		errno = EIO;
		return -1;
	}
	if (sync_error == 4) {
		sync_error = 0;
		errno = EINTR;
		return -1;
	}
	int rc = __real_fsync(fd);
	if (!rc && renamed && S_ISREG(st.st_mode) && fd != old_fd)
		new_synced = 1;
	return rc;
}
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (fd == close_error_fd) {
		close_error_fd = -1;
		errno = EIO;
		return -1;
	}
	return rc;
}
int __wrap_unlinkat(int d, const char *p, int flags)
{
	if (unlink_error && strstr(p, "20000101")) {
		errno = EACCES;
		return -1;
	}
	return __real_unlinkat(d, p, flags);
}
int __wrap_flock(int fd, int op)
{
	if (flock_eintr) {
		flock_eintr = 0;
		errno = EINTR;
		return -1;
	}
	return __real_flock(fd, op);
}
ssize_t __wrap_writev(int fd, const struct iovec *v, int n)
{
	if (vector_max)
		CHECK(n <= vector_max);
	return __real_writev(fd, v, n);
}

static void put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	CHECK(fd >= 0);
	size_t n = strlen(text);
	CHECK(write(fd, text, n) == (ssize_t)n);
	CHECK(close(fd) == 0);
}
static unsigned fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	unsigned n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			++n;
	CHECK(closedir(d) == 0);
	return n;
}
static logger_config_t config(const char *p, logger_rotate_mode_t mode)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = p;
	c.async_mode = 0;
	c.rotation.mode = mode;
	c.rotation.max_file_size = 1;
	c.rotation.retention_days = 0;
	return c;
}
static void emit(logger_t *l, const char *text)
{
	CHECK(logger_log_sync_status(l, LOGGER_INFO, "file", __FILE__, __LINE__,
				     __func__, "%s", text) == 0);
}
static void finish(logger_t *l)
{
	logger_destroy(l);
	CHECK(logger_process_object_count() == 0);
}
static int child_probe(int argc, char **argv)
{
	CHECK(argc == 5);
	logger_config_t c = config(argv[2], LOGGER_ROTATE_NONE);
	logger_t *l = logger_create(&c);
	int error = errno, busy = !strcmp(argv[3], "busy");
	CHECK(busy ? !l && error == EBUSY : l != NULL);
	if (l)
		emit(l, "child");
	if (!strcmp(argv[4], "crash"))
		_exit(0);
	logger_destroy(l);
	return 0;
}
static void probe(const char *path, const char *expect, const char *exit_mode)
{
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0);
	exe[n] = 0;
	char *args[] = {
		exe, "--probe", (char *)path, (char *)expect, (char *)exit_mode,
		NULL
	};
	pid_t p = fork();
	CHECK(p >= 0);
	if (!p) {
		execv(exe, args);
		_exit(127);
	}
	int st;
	CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
static void *race_create(void *p)
{
	logger_config_t *c = p;
	logger_t *l = logger_create(c);
	if (l) {
		CHECK(atomic_fetch_add(&wins, 1) == 0);
		winner = l;
	} else {
		CHECK(errno == EBUSY);
		atomic_fetch_add(&failures, 1);
	}
	return NULL;
}
static void owner(const char *scenario)
{
	logger_config_t c = config("out.log", LOGGER_ROTATE_NONE);
	if (!strcmp(scenario, "owner-race")) {
		pthread_t t[16];
		for (unsigned i = 0; i < 16; ++i)
			CHECK(pthread_create(t + i, NULL, race_create, &c) ==
			      0);
		for (unsigned i = 0; i < 16; ++i)
			CHECK(pthread_join(t[i], NULL) == 0);
		CHECK(atomic_load(&wins) == 1 && atomic_load(&failures) == 15);
		emit(winner, "winner");
		finish(winner);
		return;
	}
	if (!strcmp(scenario, "owner-exit")) {
		probe("out.log", "free", "crash");
		logger_t *l = logger_create(&c);
		CHECK(l);
		finish(l);
		return;
	}
	logger_t *a = logger_create(&c);
	CHECK(a);
	emit(a, "owner");
	if (!strcmp(scenario, "owner-process")) {
		probe("out.log", "busy", "close");
	} else {
		if (!strcmp(scenario, "owner-alias")) {
			CHECK(symlink(".", "alias") == 0);
			c.file_path = "alias/./out.log";
		}
		if (!strcmp(scenario, "owner-close-other")) {
			int fd = open("out.log.logger.lock", O_RDWR);
			CHECK(fd >= 0);
			CHECK(close(fd) == 0);
		}
		logger_t *b = logger_create(&c);
		CHECK(!b && errno == EBUSY);
	}
	emit(a, "still-owner");
	finish(a);
	logger_t *b = logger_create(&c);
	CHECK(b);
	finish(b);
	probe("out.log", "free", "close");
}
static void invalid_path(const char *scenario)
{
	logger_config_t c = config("out.log", LOGGER_ROTATE_NONE);
	int expected = 0;
	char long_name[5000];
	if (!strcmp(scenario, "path-empty")) {
		c.file_path = "";
		expected = EINVAL;
	} else if (!strcmp(scenario, "path-long")) {
		memset(long_name, 'a', sizeof(long_name) - 1);
		long_name[sizeof(long_name) - 1] = 0;
		c.file_path = long_name;
		expected = ENAMETOOLONG;
	} else if (!strcmp(scenario, "name-long")) {
		memset(long_name, 'a', 240);
		long_name[240] = 0;
		c.file_path = long_name;
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		expected = ENAMETOOLONG;
	} else if (!strcmp(scenario, "path-directory")) {
		CHECK(mkdir("out.log", 0700) == 0);
		expected = EINVAL;
	} else if (!strcmp(scenario, "path-fifo")) {
		CHECK(mkfifo("out.log", 0600) == 0);
		expected = EINVAL;
	} else if (!strcmp(scenario, "path-symlink")) {
		put("real.log", "sentinel");
		CHECK(symlink("real.log", "out.log") == 0);
		expected = ELOOP;
	} else if (!strcmp(scenario, "path-dangling")) {
		CHECK(symlink("absent.log", "out.log") == 0);
		expected = ELOOP;
	} else if (!strcmp(scenario, "path-hardlink")) {
		put("real.log", "sentinel");
		CHECK(link("real.log", "out.log") == 0);
		expected = EMLINK;
	} else if (!strcmp(scenario, "path-reserved-logger-lock")) {
		c.file_path = "victim.log.logger.lock";
		expected = EINVAL;
	} else if (!strcmp(scenario, "path-reserved-audit-lock")) {
		c.file_path = "victim.audit.lock";
		expected = EINVAL;
	} else if (!strcmp(scenario, "lock-symlink")) {
		put("real.lock", "sentinel");
		CHECK(symlink("real.lock", "out.log.logger.lock") == 0);
		expected = ELOOP;
	} else if (!strcmp(scenario, "lock-hardlink")) {
		put("real.lock", "sentinel");
		CHECK(link("real.lock", "out.log.logger.lock") == 0);
		expected = EMLINK;
	} else if (!strcmp(scenario, "lock-fifo")) {
		CHECK(mkfifo("out.log.logger.lock", 0600) == 0);
		expected = EINVAL;
	} else if (!strcmp(scenario, "device-rotate")) {
		c.file_path = "/dev/null";
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		expected = EINVAL;
	} else
		CHECK(0);
	unsigned count = fds();
	for (int i = 0; i < 20; ++i) {
		logger_t *l = logger_create(&c);
		CHECK(!l && errno == expected);
		CHECK(fds() == count);
	}
	CHECK(logger_process_object_count() == 0);
	if (!strcmp(scenario, "path-symlink") ||
	    !strcmp(scenario, "path-hardlink"))
		CHECK(file_size("real.log") == 8);
	if (!strcmp(scenario, "path-dangling"))
		CHECK(access("absent.log", F_OK) != 0);
}
static void reopen_case(const char *scenario)
{
	logger_config_t c = config("out.log", LOGGER_ROTATE_EXTERNAL);
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "first");
	old_fd = l->file_backend.fd;
	CHECK(rename("out.log", "old.log") == 0);
	unsigned count = fds();
	if (!strcmp(scenario, "reopen-permission"))
		data_open_error = EACCES;
	if (!strcmp(scenario, "reopen-space"))
		data_open_error = ENOSPC;
	if (!strcmp(scenario, "reopen-emfile"))
		data_open_error = EMFILE;
	if (!strcmp(scenario, "reopen-directory"))
		CHECK(mkdir("out.log", 0700) == 0);
	if (!strcmp(scenario, "reopen-symlink")) {
		put("other.log", "untouched");
		CHECK(symlink("other.log", "out.log") == 0);
	}
	if (!strcmp(scenario, "reopen-sync"))
		sync_error = 1;
	if (!strcmp(scenario, "reopen-dir-sync"))
		sync_error = 6;
	if (!strcmp(scenario, "reopen-close"))
		close_error_fd = old_fd;
	int expected = data_open_error			     ? data_open_error :
		       sync_error			     ? EIO :
		       !strcmp(scenario, "reopen-directory") ? EISDIR :
		       !strcmp(scenario, "reopen-symlink")   ? ELOOP :
		       close_error_fd >= 0		     ? EIO :
							       0;
	int rc = logger_reopen_instance(l);
	CHECK(expected ? rc == -1 && errno == expected : rc == 0);
	CHECK(fds() == count);
	if (expected && strcmp(scenario, "reopen-close")) {
		CHECK(l->file_backend.fd == old_fd &&
		      fcntl(old_fd, F_GETFD) >= 0);
		data_open_error = 0;
		sync_error = 0;
		LOGGER_INFO(l, "test", "preserved");
		CHECK(file_contains("old.log", "preserved"));
		if (!strcmp(scenario, "reopen-directory"))
			CHECK(rmdir("out.log") == 0);
		if (!strcmp(scenario, "reopen-symlink")) {
			CHECK(file_size("other.log") == 9);
			CHECK(unlink("out.log") == 0);
		}
		CHECK(logger_reopen_instance(l) == 0);
	} else
		CHECK(l->file_backend.fd != old_fd);
	LOGGER_INFO(l, "test", "new-file");
	CHECK(file_contains("out.log", "new-file"));
	if (expected)
		CHECK(logger_flush_instance_status(l) == -1 &&
		      errno == expected);
	else
		CHECK(logger_flush_instance_status(l) == 0);
	finish(l);
}
static void collision(const char *scenario)
{
	fixed_time = 1;
	frozen = (struct timespec){ 1700000000, 0 };
	const char *target = "out.20231114T221320.000000Z.log";
	put(target, "do-not-overwrite");
	logger_config_t c = config("out.log", LOGGER_ROTATE_SIZE);
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "first");
	if (!strcmp(scenario, "collision-limit"))
		rename_error = EEXIST;
	if (!strcmp(scenario, "rotation-unsupported"))
		rename_error = ENOTSUP;
	if (!strcmp(scenario, "rotation-rename-error"))
		rename_error = EACCES;
	int rc = logger_log_sync_status(l, LOGGER_INFO, "test", __FILE__,
					__LINE__, __func__, "second");
	if (rename_error) {
		CHECK(rc == -rename_error);
		CHECK(file_contains("out.log", "first") &&
		      !file_contains("out.log", "second"));
		CHECK((unsigned)rename_calls ==
		      (!strcmp(scenario, "collision-limit") ?
			       LOGGER_ARCHIVE_ATTEMPTS :
			       1));
	} else {
		CHECK(rc == 0);
		CHECK(file_contains("out.20231114T221320.000001Z.log",
				    "first"));
		if (!strcmp(scenario, "clock-backwards"))
			frozen.tv_sec -= 1000;
		emit(l, "third");
		CHECK(file_contains("out.20231114T221320.000002Z.log",
				    "second"));
		CHECK(file_contains("out.log", "third"));
	}
	CHECK(file_size(target) == (off_t)strlen("do-not-overwrite") &&
	      file_contains(target, "do-not-overwrite"));
	rename_error = 0;
	finish(l);
}
static void rotation_failure(const char *scenario)
{
	fixed_time = 1;
	logger_config_t c = config("out.log", LOGGER_ROTATE_SIZE);
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "first");
	old_fd = l->file_backend.fd;
	if (!strcmp(scenario, "rotation-new-open"))
		data_open_error = ENOSPC;
	if (!strcmp(scenario, "rotation-dir-sync"))
		sync_error = 2;
	if (!strcmp(scenario, "rotation-new-sync"))
		sync_error = 3;
	if (!strcmp(scenario, "rotation-new-dir-sync"))
		sync_error = 5;
	int expected = data_open_error ? data_open_error : EIO;
	CHECK(logger_log_sync_status(l, LOGGER_INFO, "test", __FILE__, __LINE__,
				     __func__, "unwritten") == -expected);
	CHECK(l->file_backend.fd == old_fd && l->file_backend.detached);
	const char *archive = "out.20231114T221320.000000Z.log";
	off_t size = file_size(archive);
	CHECK(file_contains(archive, "first"));
	CHECK(logger_log_sync_status(l, LOGGER_INFO, "test", __FILE__, __LINE__,
				     __func__, "not-in-archive") == -expected);
	CHECK(file_size(archive) == size);
	data_open_error = 0;
	sync_error = 0;
	CHECK(logger_reopen_instance(l) == 0 && !l->file_backend.detached);
	/* Disable size rotation only for this final probe. */
	l->file_backend.rotation.mode = LOGGER_ROTATE_NONE;
	LOGGER_INFO(l, "test", "new-active");
	CHECK(file_contains("out.log", "new-active") &&
	      file_size(archive) == size);
	CHECK(logger_flush_instance_status(l) == -1 && errno == expected);
	finish(l);
}
static void cwd_case(int rename_parent)
{
	CHECK(mkdir("a", 0700) == 0 && mkdir("b", 0700) == 0);
	logger_config_t c = config("a/out.log", LOGGER_ROTATE_SIZE);
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "old");
	if (rename_parent)
		CHECK(rename("a", "moved") == 0);
	CHECK(chdir("b") == 0);
	fixed_time = 1;
	emit(l, "after-chdir");
	CHECK(logger_reopen_instance(l) == 0);
	finish(l);
	CHECK(access("a/out.log", F_OK) != 0 && access("out.log", F_OK) != 0);
	CHECK(chdir("..") == 0);
	CHECK(file_contains(rename_parent ? "moved/out.log" : "a/out.log",
			    "after-chdir"));
}
static void stale_active(void)
{
	logger_config_t c = config("out.log", LOGGER_ROTATE_SIZE);
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "old");
	CHECK(rename("out.log", "old.log") == 0);
	put("out.log", "new-owner-data");
	CHECK(logger_log_sync_status(l, LOGGER_INFO, "m", __FILE__, __LINE__,
				     __func__, "blocked") == -ESTALE);
	CHECK(file_size("out.log") == 14 && file_contains("old.log", "old"));
	finish(l);
}
static void retention_case(int error)
{
	put("out.20000101T000000.000000Z.log", "old");
	put("out.20000101T000000.000001Z.log", "hardlink");
	CHECK(link("out.20000101T000000.000001Z.log", "keep") == 0);
	put("outside", "sentinel");
	CHECK(symlink("outside", "out.20000101T000000.000002Z.log") == 0);
	put("not-out.20000101T000000.000000Z.log", "other-owner");
	struct timespec age[2] = { { 1, 0 }, { 1, 0 } };
	CHECK(utimensat(AT_FDCWD, "out.20000101T000000.000000Z.log", age, 0) ==
	      0);
	CHECK(utimensat(AT_FDCWD, "out.20000101T000000.000001Z.log", age, 0) ==
	      0);
	logger_config_t c = config("out.log", LOGGER_ROTATE_SIZE);
	c.rotation.retention_days = 1;
	logger_t *l = logger_create(&c);
	CHECK(l);
	emit(l, "first");
	unlink_error = error;
	int rc = logger_log_sync_status(l, LOGGER_INFO, "m", __FILE__, __LINE__,
					__func__, "second");
	CHECK(error ? rc == -EACCES : rc == 0);
	CHECK(error ? access("out.20000101T000000.000000Z.log", F_OK) == 0 :
		      access("out.20000101T000000.000000Z.log", F_OK) != 0);
	CHECK(file_size("outside") == 8 && file_size("keep") == 8);
	CHECK(access("out.20000101T000000.000002Z.log", F_OK) == 0);
	CHECK(file_contains("not-out.20000101T000000.000000Z.log",
			    "other-owner"));
	unlink_error = 0;
	finish(l);
}
static void vectors(void)
{
	logger_file_t f = LOGGER_FILE_EMPTY;
	logger_rotation_config_t r = { .mode = LOGGER_ROTATE_NONE };
	CHECK(logger_file_init(&f, "out.log", r, 0600) == 0);
	vector_max = 2;
	f.iov_max = 2;
	struct iovec v[7] = { { "a", 1 }, { NULL, 0 }, { "bc", 2 }, { "", 0 },
			      { "d", 1 }, { "e", 1 },  { NULL, 0 } };
	struct timespec t = { 1700000000, 0 };
	CHECK(logger_file_writev(&f, v, 7, 5, &t, 1) == 0 &&
	      f.current_size == 5);
	CHECK(file_contains("out.log", "abcde"));
	CHECK(logger_file_writev(&f, v, 7, 99, &t, 0) == -EINVAL);
	CHECK(logger_file_close_status(&f) == 0);
}
static void cleanup(const char *path)
{
	DIR *d = opendir(path);
	CHECK(d);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char p[512];
		CHECK(snprintf(p, sizeof(p), "%s/%s", path, e->d_name) > 0);
		struct stat st;
		CHECK(lstat(p, &st) == 0);
		if (S_ISDIR(st.st_mode)) {
			cleanup(p);
			CHECK(rmdir(p) == 0);
		} else
			CHECK(unlink(p) == 0);
	}
	CHECK(closedir(d) == 0);
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--probe"))
		return child_probe(argc, argv);
	CHECK(argc == 2);
	char dir[] = "/tmp/logger-file-XXXXXX";
	enter_temp(dir);
	unsigned count = fds();
	const char *s = argv[1];
	if (!strncmp(s, "owner-", 6))
		owner(s);
	else if (!strncmp(s, "path-", 5) || !strncmp(s, "lock-", 5) ||
		 !strcmp(s, "name-long") || !strcmp(s, "device-rotate"))
		invalid_path(s);
	else if (!strncmp(s, "reopen-", 7))
		reopen_case(s);
	else if (!strncmp(s, "collision", 9) || !strcmp(s, "clock-backwards") ||
		 !strcmp(s, "rotation-unsupported") ||
		 !strcmp(s, "rotation-rename-error"))
		collision(s);
	else if (!strncmp(s, "rotation-", 9))
		rotation_failure(s);
	else if (!strcmp(s, "cwd"))
		cwd_case(0);
	else if (!strcmp(s, "directory-rename"))
		cwd_case(1);
	else if (!strcmp(s, "active-replaced"))
		stale_active();
	else if (!strcmp(s, "retention"))
		retention_case(0);
	else if (!strcmp(s, "retention-error"))
		retention_case(1);
	else if (!strcmp(s, "vectors"))
		vectors();
	else if (!strcmp(s, "eintr")) {
		flock_eintr = 1;
		logger_config_t c = config("out.log", LOGGER_ROTATE_NONE);
		logger_t *l = logger_create(&c);
		CHECK(l);
		sync_error = 4;
		emit(l, "retry");
		finish(l);
	} else if (!strcmp(s, "device-shared")) {
		logger_config_t c = config("/dev/null", LOGGER_ROTATE_NONE);
		logger_t *a = logger_create(&c), *b = logger_create(&c);
		CHECK(a && b);
		logger_destroy(a);
		logger_destroy(b);
	} else if (!strcmp(s, "init-cleanup")) {
		logger_config_t c = config("out.log", LOGGER_ROTATE_NONE);
		for (int i = 0; i < 100; ++i) {
			data_open_error = EMFILE;
			CHECK(!logger_create(&c) && errno == EMFILE);
			data_open_error = 0;
			logger_t *l = logger_create(&c);
			CHECK(l);
			finish(l);
		}
	} else
		CHECK(0);
	CHECK(fds() == count);
	cleanup(".");
	CHECK(chdir("/") == 0 && rmdir(dir) == 0);
	printf("file backend scenario %s: passed\n", s);
	return 0;
}
