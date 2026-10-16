#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_fork_fd.h"
#include "logger_process.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>

/* 故障仅位于驱动；所有 fd 与映射来自真实系统调用，不能用虚构编号代替。 */
static pid_t parent_pid;
static int descriptors[128];
static int closed_flags[128];
static unsigned descriptor_count;
static int host_fd = -1;
static int reused_fd = -1;
static int watched_close = -1;
static int child_close_count;
static int child_dir_count;
static int inject_child_eintr;
static int expected_child_error;
static size_t mapped_bytes;
static size_t baseline_bytes;
static _Atomic int fail_map, fail_remap, fail_unmap, fail_lock, fail_unlock;
static _Atomic int high_open, pause_open, pause_close, entered, release_call;
static _Atomic int fork_entered;
static _Thread_local int forking;
static _Thread_local int direct_gate;
static logger_t *created;
static int thread_result;
static int handler_hits;

int __real_open(const char *, int, ...);
int __real_openat(int, const char *, int, ...);
int __real_close(int);
int __real_socket(int, int, int);
int __real_closedir(DIR *);
void *__real_mmap(void *, size_t, int, int, int, off_t);
void *__real_mremap(void *, size_t, size_t, int, ...);
int __real_munmap(void *, size_t);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);

static void acquired_fd(int fd)
{
	if (fd < 0)
		return;
	CHECK(descriptor_count < 128);
	descriptors[descriptor_count] = fd;
	closed_flags[descriptor_count++] = 0;
}

static void consumed_fd(int fd)
{
	for (unsigned n = descriptor_count; n; --n) {
		if (descriptors[n - 1] == fd && !closed_flags[n - 1]) {
			closed_flags[n - 1] = 1;
			return;
		}
	}
}

static void hold_call(void)
{
	atomic_store(&entered, 1);
	wait_flag(&release_call);
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	int fd = __real_open(path, flags, mode);
	if (fd >= 0 && atomic_exchange(&high_open, 0)) {
		int high = fcntl(fd, F_DUPFD_CLOEXEC, 32768);
		int saved = errno;
		CHECK(__real_close(fd) == 0);
		fd = high;
		errno = saved;
	}
	acquired_fd(fd);
	return fd;
}

int __wrap_openat(int dir, const char *name, int flags, ...)
{
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	int fd = __real_openat(dir, name, flags, mode);
	acquired_fd(fd);
	if (fd >= 0 && !strcmp(name, "out.log") &&
	    atomic_exchange(&pause_open, 0))
		hold_call();
	return fd;
}

int __wrap_socket(int domain, int type, int protocol)
{
	int fd = __real_socket(domain, type, protocol);
	acquired_fd(fd);
	return fd;
}

int __wrap_close(int fd)
{
	int child = getpid() != parent_pid;
	int rc = __real_close(fd);
	if (!child)
		consumed_fd(fd);
	if (child) {
		++child_close_count;
		if (inject_child_eintr) {
			inject_child_eintr = 0;
			reused_fd =
				__real_open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (reused_fd < 0)
				_exit(88);
			errno = EINTR;
			return -1;
		}
	} else if (fd == watched_close && atomic_exchange(&pause_close, 0)) {
		hold_call();
	}
	return rc;
}

int __wrap_closedir(DIR *dir)
{
	int fd = dirfd(dir);
	if (getpid() != parent_pid)
		++child_dir_count;
	int rc = __real_closedir(dir);
	consumed_fd(fd);
	return rc;
}

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
	if (atomic_exchange(&fail_map, 0)) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	void *result = __real_mmap(p, n, prot, flags, fd, off);
	if (result != MAP_FAILED)
		mapped_bytes += n;
	return result;
}

void *__wrap_mremap(void *p, size_t old, size_t next, int flags, ...)
{
	if (atomic_exchange(&fail_remap, 0)) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	void *result = __real_mremap(p, old, next, flags);
	if (result != MAP_FAILED)
		mapped_bytes += next - old;
	return result;
}

int __wrap_munmap(void *p, size_t n)
{
	if (atomic_exchange(&fail_unmap, 0)) {
		errno = EIO;
		return -1;
	}
	int rc = __real_munmap(p, n);
	if (!rc)
		mapped_bytes -= n;
	return rc;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	if (getpid() != parent_pid)
		_exit(86);
	if (forking)
		atomic_store(&fork_entered, 1);
	if (direct_gate && atomic_exchange(&fail_lock, 0))
		return EIO;
	return __real_pthread_mutex_lock(mu);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mu)
{
	if (getpid() != parent_pid)
		_exit(87);
	int rc = __real_pthread_mutex_unlock(mu);
	if (!rc && direct_gate && atomic_exchange(&fail_unlock, 0))
		return EIO;
	return rc;
}

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void __real_free(void *);
int __real_vsnprintf(char *, size_t, const char *, va_list);
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			  void *(*)(void *), void *);

static void reject_child_unsafe(void)
{
	if (parent_pid && getpid() != parent_pid)
		_exit(85);
}

void *__wrap_malloc(size_t n)
{
	reject_child_unsafe();
	return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t size)
{
	reject_child_unsafe();
	return __real_calloc(n, size);
}

void __wrap_free(void *p)
{
	reject_child_unsafe();
	__real_free(p);
}

int __wrap_vsnprintf(char *p, size_t size, const char *fmt, va_list ap)
{
	reject_child_unsafe();
	return __real_vsnprintf(p, size, fmt, ap);
}

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
			  void *(*start)(void *), void *arg)
{
	reject_child_unsafe();
	return __real_pthread_create(t, a, start, arg);
}

static void other_handler(void)
{
	/* 早注册 handler 的 prepare 在库之后，parent 在库之前执行。 */
	errno = 0;
	if (logger_flush_instance_status(created) != -1 || errno != EDEADLK)
		_exit(84);
	++handler_hits;
}

static void wait_ok(pid_t pid)
{
	int status = 0;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "CHILD_STATUS=%d\n", status);
		CHECK(0);
	}
}

static void child_probe(void)
{
	/* fork 子方不调用 CHECK/stdio/分配器或旧实例 API。 */
	for (unsigned i = 0; i < descriptor_count; ++i) {
		int fd = descriptors[i];
		if (!closed_flags[i] && fd != reused_fd &&
		    fcntl(fd, F_GETFD) >= 0)
			_exit(30);
	}
	if (host_fd >= 0 && fcntl(host_fd, F_GETFD) < 0)
		_exit(31);
	if (reused_fd >= 0 && fcntl(reused_fd, F_GETFD) < 0)
		_exit(32);
	if (logger_fork_fd_child_error() != expected_child_error)
		_exit(33);
	if (child_dir_count)
		_exit(34);
	/* 子方未初始化的普通入口仍拒绝；新初始化事务由独立用例验证，
	 * 这里继续严格禁止 child 回调内发生分配、同步或格式化。
	 */
	if (logger_process_ensure() != -ECHILD)
		_exit(35);
}

static void fork_probe(void)
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		child_probe();
		_exit(0);
	}
	wait_ok(pid);
}

static int owned_null(void)
{
	return logger_fork_open("/dev/null", O_RDONLY | O_CLOEXEC, 0);
}

static void descriptor_case(int nested)
{
	int a = owned_null();
	int b = logger_fork_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int d = logger_fork_open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	CHECK(a >= 0 && b >= 0 && d >= 0);
	DIR *dir = logger_fork_fdopendir(d);
	CHECK(dir);
	/* 目录租约现在必须在 fork 前完整消费；跨线程持有场景另有
	 * directory-held 正向验证，不能继承 libc DIR 缓冲。
	 */
	CHECK(logger_fork_closedir(dir) == 0);
	if (!nested)
		fork_probe();
	else {
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			child_probe();
			int count = child_close_count;
			pid_t next = fork();
			if (next < 0)
				_exit(36);
			if (!next)
				_exit(child_close_count == count ? 0 : 37);
			int status = 0;
			if (waitpid(next, &status, 0) != next || status)
				_exit(38);
			_exit(0);
		}
		wait_ok(pid);
	}
	CHECK(fcntl(a, F_GETFD) >= 0 && fcntl(b, F_GETFD) >= 0);
	CHECK(logger_fork_close(a) == 0);
	CHECK(logger_fork_close(b) == 0);
	CHECK(mapped_bytes == baseline_bytes);
}

static void *create_thread(void *unused)
{
	(void)unused;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.queue_capacity = 8;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	created = logger_create(&cfg);
	return NULL;
}

static void *fork_thread(void *unused)
{
	(void)unused;
	forking = 1;
	fork_probe();
	forking = 0;
	return NULL;
}

static void *close_thread(void *unused)
{
	(void)unused;
	thread_result = logger_fork_close(watched_close);
	return NULL;
}

static void window(int closing)
{
	pthread_t worker, forker;
	if (closing) {
		watched_close = owned_null();
		CHECK(watched_close >= 0);
		atomic_store(&pause_close, 1);
		CHECK(!pthread_create(&worker, NULL, close_thread, NULL));
	} else {
		atomic_store(&pause_open, 1);
		CHECK(!pthread_create(&worker, NULL, create_thread, NULL));
	}
	wait_flag(&entered);
	if (closing) {
		int fd = __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
		CHECK(fd >= 0);
		if (fd != watched_close) {
			CHECK(dup2(fd, watched_close) == watched_close);
			CHECK(!__real_close(fd));
		}
		reused_fd = watched_close;
	}
	CHECK(!pthread_create(&forker, NULL, fork_thread, NULL));
	/* 确认 forker 已进入 prepare 的真实锁调用后才放行资源窗口。 */
	wait_flag(&fork_entered);
	atomic_store(&release_call, 1);
	CHECK(!pthread_join(worker, NULL));
	CHECK(!pthread_join(forker, NULL));
	if (closing) {
		CHECK(!thread_result);
		CHECK(fcntl(reused_fd, F_GETFD) >= 0);
		CHECK(!__real_close(reused_fd));
	} else {
		CHECK(created);
		LOGGER_INFO(created, "fork", "PARENT_AFTER_CANDIDATE");
		CHECK(!logger_destroy_status(created));
		CHECK(file_contains("out.log", "PARENT_AFTER_CANDIDATE"));
	}
	CHECK(mapped_bytes == baseline_bytes);
}

static void file_lock_case(void)
{
	create_thread(NULL);
	CHECK(created && descriptor_count == 3);
	int check = __real_open("out.log.logger.lock", O_RDWR | O_CLOEXEC);
	CHECK(check >= 0);
	int ready[2], leave[2];
	CHECK(!pipe(ready) && !pipe(leave));
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		child_probe();
		char c = 1;
		if (write(ready[1], &c, 1) != 1 || read(leave[0], &c, 1) != 1)
			_exit(40);
		_exit(0);
	}
	char c;
	CHECK(read(ready[0], &c, 1) == 1);
	CHECK(flock(check, LOCK_EX | LOCK_NB) == -1);
	CHECK(errno == EWOULDBLOCK || errno == EAGAIN);
	LOGGER_INFO(created, "fork", "PARENT_LOCK_STILL_OWNED");
	CHECK(!logger_destroy_status(created));
	CHECK(file_contains("out.log", "PARENT_LOCK_STILL_OWNED"));
	/* 子方仍然存活：父方关闭后可取得锁，证明子方没有残留共享引用。 */
	CHECK(!flock(check, LOCK_EX | LOCK_NB));
	CHECK(write(leave[1], &c, 1) == 1);
	wait_ok(pid);
	CHECK(!__real_close(check));
	for (unsigned i = 0; i < 2; ++i) {
		CHECK(!__real_close(ready[i]));
		CHECK(!__real_close(leave[i]));
	}
	CHECK(mapped_bytes == baseline_bytes);
}

static void syslog_case(void)
{
	int server = __real_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(server >= 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	memcpy(address.sun_path, "devlog.sock", sizeof("devlog.sock"));
	CHECK(!bind(server, (struct sockaddr *)&address, sizeof(address)));
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_SYSLOG;
	cfg.syslog.path = "devlog.sock";
	cfg.queue_capacity = 8;
	logger_t *l = logger_create(&cfg);
	CHECK(l && descriptor_count == 1);
	fork_probe();
	LOGGER_INFO(l, "fork", "PARENT_SYSLOG_CONTINUES");
	CHECK(!logger_flush_instance_status(l));
	char packet[1024];
	ssize_t n = recv(server, packet, sizeof(packet) - 1, MSG_DONTWAIT);
	CHECK(n > 0);
	packet[n] = 0;
	CHECK(strstr(packet, "PARENT_SYSLOG_CONTINUES"));
	CHECK(!logger_destroy_status(l));
	CHECK(!__real_close(server));
	CHECK(!unlink("devlog.sock"));
	CHECK(mapped_bytes == baseline_bytes);
}

static void mapping_case(int grow, int fail)
{
	int first = -1;
	if (grow) {
		struct rlimit limit;
		CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
		CHECK(limit.rlim_max > 32768);
		if (limit.rlim_cur <= 32768) {
			limit.rlim_cur = 32769;
			CHECK(!setrlimit(RLIMIT_NOFILE, &limit));
		}
		first = owned_null();
		CHECK(first >= 0);
		atomic_store(&high_open, 1);
		atomic_store(&fail_remap, fail);
	} else
		atomic_store(&fail_map, fail);
	int fd = owned_null();
	if (fail) {
		CHECK(fd == -1 && errno == ENOMEM);
		CHECK(closed_flags[descriptor_count - 1]);
		fd = owned_null();
	}
	CHECK(fd >= 0);
	fork_probe();
	CHECK(!logger_fork_close(fd));
	if (first >= 0)
		CHECK(!logger_fork_close(first));
	CHECK(mapped_bytes == baseline_bytes);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (!strcmp(argv[1], "grow") || !strcmp(argv[1], "grow-failure")) {
		struct rlimit limit;
		CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
		if (limit.rlim_max <= 32768) {
			printf("FORK_FD_SKIP=high-fd hard=%llu\n",
			       (unsigned long long)limit.rlim_max);
			return 77;
		}
	}
	parent_pid = getpid();
	if (!strcmp(argv[1], "handler-order"))
		CHECK(!pthread_atfork(other_handler, other_handler, NULL));
	CHECK(!logger_process_init());
	baseline_bytes = mapped_bytes;
	host_fd = __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
	CHECK(host_fd >= 0);
	char temp[] = "/tmp/logger-fork-fd-XXXXXX";
	enter_temp(temp);
	const char *mode = argv[1];
	if (!strcmp(mode, "syslog"))
		syslog_case();
	else if (!strcmp(mode, "handler-order")) {
		create_thread(NULL);
		CHECK(created);
		fork_probe();
		CHECK(handler_hits == 2);
		CHECK(!logger_destroy_status(created));
	} else if (!strcmp(mode, "descriptors"))
		descriptor_case(0);
	else if (!strcmp(mode, "nested"))
		descriptor_case(1);
	else if (!strcmp(mode, "child-eintr")) {
		inject_child_eintr = 1;
		expected_child_error = EINTR;
		descriptor_case(0);
	} else if (!strcmp(mode, "open-window"))
		window(0);
	else if (!strcmp(mode, "close-reuse-window"))
		window(1);
	else if (!strcmp(mode, "file-lock"))
		file_lock_case();
	else if (!strcmp(mode, "map-failure"))
		mapping_case(0, 1);
	else if (!strcmp(mode, "grow-failure"))
		mapping_case(1, 1);
	else if (!strcmp(mode, "grow"))
		mapping_case(1, 0);
	else if (!strcmp(mode, "foreign-fd")) {
		CHECK(logger_fork_close(host_fd) == -1 && errno == EBADF);
		CHECK(fcntl(host_fd, F_GETFD) >= 0);
		CHECK(!logger_fork_fdopendir(host_fd) && errno == EBADF);
		CHECK(mapped_bytes == baseline_bytes);
	} else if (!strcmp(mode, "parent-unwind")) {
		/* 只验证回调收尾合同，不冒称注入真实 fork 系统调用失败。 */
		logger_fork_fd_prepare();
		errno = EAGAIN;
		logger_fork_fd_parent();
		CHECK(errno == EAGAIN);
		int fd = owned_null();
		CHECK(fd >= 0 && !logger_fork_close(fd));
	} else if (!strcmp(mode, "unmap-failure")) {
		int fd = owned_null();
		CHECK(fd >= 0);
		atomic_store(&fail_unmap, 1);
		CHECK(logger_fork_close(fd) == -1 && errno == EIO);
		CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
		expected_child_error = EIO;
		fork_probe();
		CHECK(owned_null() == -1 && errno == EIO);
	} else if (!strcmp(mode, "lock-failure")) {
		direct_gate = 1;
		atomic_store(&fail_lock, 1);
		CHECK(owned_null() == -1 && errno == EIO);
		direct_gate = 0;
		CHECK(!descriptor_count);
		expected_child_error = EIO;
		fork_probe();
	} else
		CHECK(0);
	CHECK(!__real_close(host_fd));
	/* 只有文件用例产生这两个目标；其余用例也拥有独立目录。 */
	if (!access("out.log", F_OK))
		leave_temp(temp);
	else {
		CHECK(!chdir("/"));
		CHECK(!rmdir(temp));
	}
	printf("FORK_FD_PASS=%s\n", mode);
	return 0;
}
