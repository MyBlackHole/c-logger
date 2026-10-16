#define _GNU_SOURCE
#include "support.h"
#include "logger.h"
#include "logger_internal.h"
#include "logger_runtime.h"
#include "logger_fork_fd.h"
#include "logger_libc.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* 仅包装当前生产归档实际依赖；故障不通过改写库内状态产生。 */
enum failure {
	FAIL_NONE,
	FAIL_MAP,
	FAIL_ADVICE,
	FAIL_MUTEX,
	FAIL_RWLOCK,
	FAIL_UNMAP
};
static int fault;
static int allocation_errno = ENOMEM;
static _Atomic unsigned mapping_calls;
static unsigned countdown;
static _Atomic int entered;
static _Atomic int release_holder;
static _Atomic int fork_waiting;
static _Atomic int fork_done;
static _Thread_local int in_fork;
static int hold_format;
static int child_async = 1;
static int fail_final_unlock;
static int arm_unlock;
static uintptr_t final_subject;
static const char *hold_mode;
static logger_t *parent_logger;
static void *old_regions[5];

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_madvise(void *, size_t, int);
int __real_munmap(void *, size_t);
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __real_pthread_rwlock_init(pthread_rwlock_t *,
			       const pthread_rwlockattr_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
struct tm *__real_localtime_r(const time_t *, struct tm *);

static int failing(int which)
{
	if (fault != which || !countdown)
		return 0;
	if (--countdown)
		return 0;
	fault = FAIL_NONE;
	return 1;
}

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
	atomic_fetch_add(&mapping_calls, 1);
	if (failing(FAIL_MAP)) {
		errno = allocation_errno;
		return MAP_FAILED;
	}
	return __real_mmap(p, n, prot, flags, fd, off);
}

int __wrap_madvise(void *p, size_t n, int advice)
{
	if (failing(FAIL_ADVICE)) {
		errno = allocation_errno;
		return -1;
	}
	return __real_madvise(p, n, advice);
}

int __wrap_munmap(void *p, size_t n)
{
	if (failing(FAIL_UNMAP)) {
		errno = EIO;
		return -1;
	}
	int rc = __real_munmap(p, n);
	if (!rc && fail_final_unlock && (uintptr_t)p <= final_subject &&
	    final_subject - (uintptr_t)p < n)
		arm_unlock = 1;
	return rc;
}

int __wrap_pthread_mutex_init(pthread_mutex_t *p,
			      const pthread_mutexattr_t *attr)
{
	return failing(FAIL_MUTEX) ? EAGAIN :
				     __real_pthread_mutex_init(p, attr);
}

int __wrap_pthread_rwlock_init(pthread_rwlock_t *p,
			       const pthread_rwlockattr_t *attr)
{
	return failing(FAIL_RWLOCK) ? EAGAIN :
				      __real_pthread_rwlock_init(p, attr);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *p)
{
	if (in_fork)
		atomic_store(&fork_waiting, 1);
	return __real_pthread_mutex_lock(p);
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *p)
{
	int rc = __real_pthread_mutex_unlock(p);
	if (!rc && arm_unlock) {
		arm_unlock = 0;
		fail_final_unlock = 0;
		return EIO;
	}
	return rc;
}

struct tm *__wrap_localtime_r(const time_t *time, struct tm *out)
{
	if (hold_format) {
		atomic_store(&entered, 1);
		wait_flag(&release_holder);
	}
	return __real_localtime_r(time, out);
}

static void child_check(int yes, int code)
{
	if (!yes)
		_exit(code);
}

static void wait_ok(pid_t pid)
{
	int status;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		fprintf(stderr, "CHILD_STATUS=%d\n", status);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static logger_config_t cfg_for(const char *path)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = path;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 8;
	return cfg;
}

static void child_log(void)
{
	logger_config_t cfg = cfg_for("child.log");
	cfg.async_mode = child_async;
	logger_t *l = NULL;
	(void)logger_create(&cfg, &l);
	child_check(l != NULL, 41);
	LOGGER_INFO(l, "child", "NEW_RUNTIME_RECORD");
	child_check(logger_destroy(&l) == 0, 42);
	child_check(logger_runtime_current()->allocation_count == 0, 43);
	child_check(logger_process_object_count() == 0, 44);
	_exit(0);
}

static void check_unmapped(void)
{
	/* 在创建任何新域前确认旧页不存在，避免地址复用造成伪阳性。 */
	long page = sysconf(_SC_PAGESIZE);
	child_check(page > 0, 45);
	for (unsigned i = 0; i < sizeof(old_regions) / sizeof(old_regions[0]);
	     ++i) {
		unsigned char resident;
		uintptr_t address = (uintptr_t)old_regions[i];
		address -= address % (uintptr_t)page;
		child_check(mincore((void *)address, (size_t)page, &resident) ==
					    -1 &&
				    errno == ENOMEM,
			    46);
	}
	child_check(logger_runtime_current() == NULL, 47);
}

static void mapped_and_retry(const char *mode)
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		check_unmapped();
		if (!strcmp(mode, "map-error"))
			fault = FAIL_MAP;
		else if (!strcmp(mode, "advice-error"))
			fault = FAIL_ADVICE;
		else if (!strncmp(mode, "mutex-", 6))
			fault = FAIL_MUTEX;
		else if (!strcmp(mode, "rwlock-error"))
			fault = FAIL_RWLOCK;
		else if (!strcmp(mode, "bitmap-unmap-error"))
			fault = FAIL_UNMAP;
		if (fault) {
			countdown = fault == FAIL_MUTEX ?
					    (unsigned)(mode[6] - '0') :
					    1;
			errno = EDOM;
			int rc = logger_process_init();
			child_check(rc < 0 && errno == EDOM && countdown == 0,
				    48);
			child_check(logger_runtime_current() == NULL, 49);
			logger_config_t cfg = cfg_for("never-created.log");
			child_check(logger_create(&cfg,
						  &(logger_t *){ NULL }) ==
					    -ECHILD,
				    50);
		}
		child_check(logger_process_init() == 0, 51);
		child_check(logger_process_init() == -EALREADY, 52);
		child_check(logger_runtime_current()->allocation_count == 0,
			    53);
		child_log();
	}
	wait_ok(pid);
	CHECK(file_contains("child.log", "NEW_RUNTIME_RECORD"));
	CHECK(!file_contains("child.log", "PARENT_ONLY"));
}

static void *holder(void *unused)
{
	(void)unused;
	if (!strcmp(hold_mode, "directory-held")) {
		int fd = logger_fork_open(".", O_RDONLY | O_DIRECTORY, 0);
		CHECK(fd >= 0);
		DIR *dir = logger_fork_fdopendir(fd);
		CHECK(dir);
		atomic_store(&entered, 1);
		wait_flag(&release_holder);
		CHECK(logger_fork_closedir(dir) == 0);
	} else if (!strcmp(hold_mode, "format-held")) {
		time_t time = 0;
		struct tm out;
		CHECK(logger_libc_localtime_r(&time, &out) == &out);
	} else {
		CHECK(pthread_mutex_lock(&parent_logger->emit_mu) == 0);
		atomic_store(&entered, 1);
		wait_flag(&release_holder);
		CHECK(pthread_mutex_unlock(&parent_logger->emit_mu) == 0);
	}
	return NULL;
}

static void *forker(void *unused)
{
	(void)unused;
	in_fork = 1;
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		hold_format = 0;
		child_check(logger_process_init() == 0, 54);
		child_log();
	}
	in_fork = 0;
	wait_ok(pid);
	atomic_store(&fork_done, 1);
	return NULL;
}

static void interleaving(const char *mode)
{
	hold_mode = mode;
	hold_format = !strcmp(mode, "format-held");
	pthread_t holding;
	pthread_t fork_thread;
	CHECK(pthread_create(&holding, NULL, holder, NULL) == 0);
	wait_flag(&entered);
	CHECK(pthread_create(&fork_thread, NULL, forker, NULL) == 0);
	if (!strcmp(mode, "emit-held")) {
		/* 父方实例锁仍被占用时，子方已完成独立初始化和写入。 */
		wait_flag(&fork_done);
	} else {
		wait_flag(&fork_waiting);
		CHECK(!atomic_load(&fork_done));
	}
	atomic_store(&release_holder, 1);
	CHECK(pthread_join(holding, NULL) == 0);
	CHECK(pthread_join(fork_thread, NULL) == 0);
	hold_format = 0;
	CHECK(file_contains("child.log", "NEW_RUNTIME_RECORD"));
}

static void early_child(void)
{
	child_check(logger_process_init() == -ECHILD, 55);
}

static void allocation_failures(void)
{
	struct logger_runtime *runtime = logger_runtime_current();
	size_t before = runtime->allocation_count;
	size_t owned_before = runtime->owned_count;
	logger_config_t cfg = cfg_for("failed.log");
	const int failures[] = { FAIL_MAP, FAIL_ADVICE };
	const int errors[] = { ENOMEM, EIO };
	unsigned cases = 0;

	/* 一个实例、两个队列数组、七个工作区映射，逐点拒绝真实系统调用。
	 * 非 ENOMEM 错误必须原样返回；首错后不再取得后续候选映射。
	 */
	for (unsigned f = 0; f < sizeof(failures) / sizeof(failures[0]); ++f) {
		for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]);
		     ++e) {
			for (unsigned i = 1; i <= 10; ++i) {
				allocation_errno = errors[e];
				fault = failures[f];
				countdown = i;
				atomic_store(&mapping_calls, 0);
				logger_t *l = NULL;
				int create_rc = logger_create(&cfg, &l);
				int error = -create_rc;

				CHECK(l == NULL && countdown == 0);
				CHECK(error == allocation_errno);
				CHECK(atomic_load(&mapping_calls) == i);
				CHECK(runtime->allocation_count == before);
				CHECK(runtime->owned_count == owned_before);
				CHECK(logger_process_object_count() == 1);
				/* 失败后同一进程、同一路径可真正创建与释放。 */
				CHECK(logger_create(&cfg, &l) == 0);
				CHECK(l != NULL);
				CHECK(logger_destroy(&l) == 0);
				CHECK(runtime->allocation_count == before);
				CHECK(runtime->owned_count == owned_before);
				++cases;
			}
		}
	}
	allocation_errno = ENOMEM;
	CHECK(cases == 40);
	printf("ALLOCATION_FAILURE_CASES=%u\n", cases);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	(void)alarm(20);
	char path[] = "/tmp/logger-runtime-test-XXXXXX";
	enter_temp(path);
	char normalized[64];
	CHECK(strlen(argv[1]) < sizeof(normalized));
	memcpy(normalized, argv[1], strlen(argv[1]) + 1);
	size_t length = strlen(normalized);
	if (length > 5 && !strcmp(normalized + length - 5, "-sync")) {
		normalized[length - 5] = 0;
		child_async = 0;
	}
	const char *mode = normalized;
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
	if (child_async && strcmp(mode, "allocation-errors") &&
	    strcmp(mode, "unmap-retain") && strcmp(mode, "final-unlock") &&
	    strcmp(mode, "raw-bypass")) {
		printf("FORK_RUNTIME_SKIP=tsan-post-fork-worker case=%s\n",
		       argv[1]);
		CHECK(chdir("/") == 0);
		CHECK(rmdir(path) == 0);
		return 77;
	}
#endif
	if (!strcmp(mode, "earlier-handler"))
		CHECK(pthread_atfork(NULL, NULL, early_child) == 0);
	CHECK(logger_process_init() == 0);
	logger_config_t cfg = cfg_for("out.log");
	CHECK(logger_create(&cfg, &parent_logger) == 0);
	CHECK(parent_logger != NULL);
	LOGGER_INFO(parent_logger, "parent", "PARENT_ONLY");
	CHECK(logger_flush(parent_logger) == 0);
	old_regions[0] = logger_runtime_current();
	old_regions[1] = parent_logger;
	old_regions[2] = parent_logger->q.slots;
	old_regions[3] = parent_logger->q.spills;
	old_regions[4] = parent_logger->worker_workspace;
	if (!strcmp(mode, "final-unlock")) {
		final_subject = (uintptr_t)parent_logger;
		fail_final_unlock = 1;
		CHECK(logger_destroy(&parent_logger) == -EIO);
		CHECK(logger_destroy_receipt_read() == 1);
		CHECK(logger_runtime_current()->allocation_count == 0);
		CHECK(!fail_final_unlock && !arm_unlock);
		leave_temp(path);
		printf("FORK_RUNTIME_PASS=%s consumed=1\n", mode);
		return 0;
	} else if (strstr(mode, "-held")) {
		interleaving(mode);
	} else if (!strcmp(mode, "allocation-errors")) {
		allocation_failures();
	} else if (!strcmp(mode, "unmap-retain")) {
		CHECK(logger_destroy(&parent_logger) == 0);
		void *p = logger_runtime_malloc(32);
		CHECK(p);
		fault = FAIL_UNMAP;
		countdown = 1;
		CHECK(logger_runtime_free(p) == -EIO);
		CHECK(logger_runtime_current()->allocation_count == 1);
		CHECK(logger_runtime_malloc(32) == NULL && errno == EIO);
		/* 故障状态按合同保留映射，不能再假装执行正常回收。 */
		leave_temp(path);
		printf("FORK_RUNTIME_PASS=%s retained=1\n", mode);
		return 0;
	} else if (!strcmp(mode, "raw-bypass")) {
		pid_t pid = (pid_t)syscall(SYS_fork);
		CHECK(pid >= 0);
		if (!pid)
			_exit(logger_process_init() == -ECHILD ? 0 : 56);
		wait_ok(pid);
	} else {
		CHECK(!strcmp(mode, "mapped") || !strcmp(mode, "map-error") ||
		      !strcmp(mode, "advice-error") ||
		      !strcmp(mode, "rwlock-error") ||
		      !strcmp(mode, "mutex-1") || !strcmp(mode, "mutex-2") ||
		      !strcmp(mode, "mutex-3") || !strcmp(mode, "mutex-4") ||
		      !strcmp(mode, "bitmap-unmap-error") ||
		      !strcmp(mode, "earlier-handler"));
		mapped_and_retry(mode);
	}
	LOGGER_INFO(parent_logger, "parent", "PARENT_AFTER");
	CHECK(logger_destroy(&parent_logger) == 0);
	CHECK(file_contains("out.log", "PARENT_AFTER"));
	CHECK(logger_runtime_current()->allocation_count == 0);
	CHECK(logger_process_object_count() == 0);
	const char *extra[] = { "child.log", "child.log.logger.lock",
				"failed.log", "failed.log.logger.lock",
				"never-created.log.logger.lock" };
	for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
		CHECK(unlink(extra[i]) == 0 || errno == ENOENT);
	leave_temp(path);
	printf("FORK_RUNTIME_PASS=%s child_async=%d\n", argv[1], child_async);
	return 0;
}
