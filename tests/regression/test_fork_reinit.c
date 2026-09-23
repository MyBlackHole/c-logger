#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include "audit.h"
#include "console.h"
#undef CONSOLE_DEBUG
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>

static int observe_fork, fail_fork, fail_proc, fail_close, fail_alloc,
	fail_register;
static int old_fd = -1;
static unsigned fork_calls;
static logger_config_t cfg;

pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
	++fork_calls;
	if (observe_fork) {
		unsigned threads;
		CHECK(logger_process_thread_count(&threads) == 0 &&
		      threads == 1);
		CHECK(logger_process_object_count() == 0);
		CHECK(logger_process_ensure() ==
		      -EBUSY); /* armed interval is closed */
		if (old_fd >= 0)
			CHECK(fcntl(old_fd, F_GETFD) == -1 && errno == EBADF);
	}
	if (fail_fork) {
		errno = EAGAIN;
		return -1;
	}
	return __real_fork();
}
DIR *__real_opendir(const char *);
DIR *__wrap_opendir(const char *name)
{
	if (fail_proc && !strcmp(name, "/proc/self/task")) {
		errno = EACCES;
		return NULL;
	}
	return __real_opendir(name);
}
int __real_close(int);
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (fail_close && fd == old_fd) {
		fail_close = 0;
		errno = EIO;
		return -1;
	}
	return rc;
}
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t z)
{
	if (fail_alloc && n == 1 && z == sizeof(logger_t)) {
		fail_alloc = 0;
		errno = ENOMEM;
		return NULL;
	}
	return __real_calloc(n, z);
}
int __real_pthread_atfork(void (*)(void), void (*)(void), void (*)(void));
int __wrap_pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void))
{
	if (fail_register)
		return ENOMEM;
	return __real_pthread_atfork(a, b, c);
}

static logger_config_t config(const char *path, int async)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = async;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 2048;
	c.flush_level = LOGGER_OFF;
	return c;
}
static audit_config_t audit_config(const char *name)
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = name;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	return c;
}
static void wait_child(pid_t p)
{
	int st;
	while (waitpid(p, &st, 0) < 0)
		CHECK(errno == EINTR);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}
static int find_fd(const char *path)
{
	struct stat want, st;
	CHECK(stat(path, &want) == 0);
	for (int fd = 3; fd < 256; ++fd)
		if (fstat(fd, &st) == 0 && want.st_dev == st.st_dev &&
		    want.st_ino == st.st_ino)
			return fd;
	CHECK(!"logger file fd not found");
	return -1;
}
static void audit_one(const char *name)
{
	audit_config_t c = audit_config(name);
	CHECK(audit_init(&c) == 0);
	audit_event_t e = { .event = "FORK_REINIT", .operation = "check" };
	CHECK(audit_begin(&e) == 0);
	CHECK(audit_end(&e, AUDIT_SUCCESS, 0) == 0);
	CHECK(audit_shutdown_status() == 0);
	char path[128];
	CHECK(snprintf(path, sizeof(path), "%s.audit.log", name) > 0);
	CHECK(audit_verify_file(path) == 0);
}
static void one_pair(void)
{
	observe_fork = 1;
	pid_t p = logger_fork_reinit();
	CHECK(p >= 0);
	observe_fork = 0;
	CHECK(logger_process_object_count() == 0);
	errno = 0;
	/* The helper completed a global shutdown: same phase as normal STOPPED. */
	CHECK(logger_reopen() == -1 && errno == ESHUTDOWN);
	if (!p) {
		logger_context_t c = logger_context_get();
		CHECK(!c.request_id && !c.session_id && !c.trace_id);
		logger_config_t child = config("child.after.log", 1);
		CHECK(logger_init(&child) == 0);
		LOG_INFO("CHILD_ONLY");
		CHECK(logger_flush_status() == 0);
		console_config_t cc = CONSOLE_DEFAULT_CONFIG();
		cc.verbosity = CONSOLE_QUIET;
		cc.color = CONSOLE_COLOR_NEVER;
		errno = 0;
		console_init(&cc);
		CHECK(errno != ECHILD);
		CHECK(console_info("hidden") == 0);
		logger_shutdown();
		audit_one("child");
		CHECK(logger_process_object_count() == 0);
		exit(0); /* controlled clean fork, no inherited runtime allocation */
	}
	logger_config_t parent = config("parent.after.log", 1);
	CHECK(logger_init(&parent) == 0);
	LOG_INFO("PARENT_ONLY");
	CHECK(logger_flush_status() == 0);
	logger_shutdown();
	audit_one("parent");
	wait_child(p);
	CHECK(file_contains("child.after.log", "CHILD_ONLY"));
	CHECK(!file_contains("child.after.log", "PARENT_ONLY"));
	CHECK(file_contains("parent.after.log", "PARENT_ONLY"));
	CHECK(!file_contains("parent.after.log", "CHILD_ONLY"));
	CHECK(logger_process_object_count() == 0);
}
static _Atomic int thread_ready, thread_done;
static void *idle_thread(void *unused)
{
	(void)unused;
	atomic_store(&thread_ready, 1);
	wait_flag(&thread_done);
	return NULL;
}
static void before_parent_handler(void)
{
	errno = 0;
	CHECK(logger_init(&cfg) == -1 && errno == EBUSY);
}
static void before_child_handler(void)
{
	errno = 0;
	if (logger_init(&cfg) != -1 || errno != ECHILD)
		_exit(97);
}
static void after_drain_test(void)
{
	cfg = config("pending.log", 1);
	CHECK(logger_init(&cfg) == 0);
	old_fd = find_fd("pending.log");
	for (unsigned i = 0; i < 1000; ++i)
		LOG_INFO("PRE_FORK_RECORD_%04u", i);
	one_pair();
	FILE *f = fopen("pending.log", "r");
	CHECK(f != NULL);
	char line[1024], marker[80];
	unsigned n = 0;
	while (fgets(line, sizeof(line), f)) {
		CHECK(n < 1000);
		snprintf(marker, sizeof(marker), "PRE_FORK_RECORD_%04u", n++);
		CHECK(strstr(line, marker) != NULL);
	}
	CHECK(!ferror(f) && n == 1000);
	CHECK(fclose(f) == 0);
}
static void guard_after_raw(void)
{
	pid_t p = fork();
	CHECK(p >= 0);
	if (!p) {
		errno = 0;
		if (logger_fork_reinit() != -1 || errno != ECHILD)
			_exit(91);
		logger_after_fork_child();
		errno = 0;
		if (logger_init(&cfg) != -1 || errno != ECHILD)
			_exit(92);
		_exit(0);
	}
	wait_child(p);
}
static void run(const char *mode)
{
	cfg = config("before.log", 1);
	if (!strcmp(mode, "drain")) {
		after_drain_test();
		return;
	}
	if (!strcmp(mode, "no-init")) {
		one_pair();
		return;
	}
	if (!strcmp(mode, "console-only")) {
		console_config_t c = CONSOLE_DEFAULT_CONFIG();
		console_init(&c);
		one_pair();
		return;
	}
	if (!strcmp(mode, "registration-failure")) {
		fail_register = 1;
		errno = 0;
		CHECK(logger_fork_reinit() == -1 && errno == ENOMEM &&
		      fork_calls == 0);
		CHECK(logger_process_object_count() == 0);
		return;
	}
	if (!strcmp(mode, "creation-failure")) {
		fail_alloc = 1;
		CHECK(logger_create(&cfg) == NULL && errno == ENOMEM);
		CHECK(logger_process_object_count() == 0);
		logger_config_t bad = config("missing/dir/log", 0);
		CHECK(logger_create(&bad) == NULL);
		CHECK(logger_process_object_count() == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "explicit-only")) {
		logger_t *l = logger_create(&cfg);
		CHECK(l != NULL);
		CHECK(logger_fork_reinit() == -1 && errno == EBUSY &&
		      !fork_calls);
		CHECK(logger_get_state(l) == LOGGER_STATE_RUNNING);
		logger_destroy(l);
		one_pair();
		return;
	}
	if (!strcmp(mode, "earlier-handler"))
		CHECK(pthread_atfork(NULL, before_parent_handler,
				     before_child_handler) == 0);
	if (!strcmp(mode, "sync"))
		cfg.async_mode = 0;
	if (!strcmp(mode, "io-failure"))
		cfg.file_path = "/dev/full";
	CHECK(logger_init(&cfg) == 0);
	LOG_INFO("BEFORE_FORK");
	if (!strcmp(mode, "async") || !strcmp(mode, "sync") ||
	    !strcmp(mode, "earlier-handler")) {
		one_pair();
		return;
	}
	if (!strcmp(mode, "after-shutdown")) {
		CHECK(logger_flush_status() == 0);
		logger_shutdown();
		one_pair();
		return;
	}
	if (!strcmp(mode, "context")) {
		logger_context_set(
			&(logger_context_t){ .request_id = "PARENT_CONTEXT" });
		one_pair();
		CHECK(strcmp(logger_context_get().request_id,
			     "PARENT_CONTEXT") == 0);
		return;
	}
	if (!strcmp(mode, "raw-guard")) {
		guard_after_raw();
		CHECK(logger_flush_status() == 0);
		logger_shutdown();
		return;
	}
	if (!strcmp(mode, "thread-busy")) {
		pthread_t t;
		CHECK(pthread_create(&t, NULL, idle_thread, NULL) == 0);
		wait_flag(&thread_ready);
		CHECK(logger_fork_reinit() == -1 && errno == EBUSY &&
		      !fork_calls);
		CHECK(logger_flush_status() == 0);
		atomic_store(&thread_done, 1);
		CHECK(pthread_join(t, NULL) == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "explicit-busy")) {
		logger_config_t e = config("explicit.log", 1);
		logger_t *l = logger_create(&e);
		CHECK(l != NULL);
		CHECK(logger_fork_reinit() == -1 && errno == EBUSY &&
		      !fork_calls);
		CHECK(logger_flush_status() == 0);
		LOGGER_INFO(l, "explicit", "UNCHANGED");
		CHECK(logger_flush_instance_status(l) == 0);
		logger_destroy(l);
		one_pair();
		return;
	}
	if (!strcmp(mode, "audit-busy")) {
		audit_config_t a = audit_config("audit-before");
		CHECK(audit_init(&a) == 0);
		char before[33], after[33];
		CHECK(audit_instance_id_copy(before) == 0);
		CHECK(logger_fork_reinit() == -1 && errno == EBUSY &&
		      !fork_calls);
		CHECK(logger_flush_status() == 0);
		CHECK(audit_instance_id_copy(after) == 0 &&
		      !strcmp(before, after));
		CHECK(audit_shutdown_status() == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "proc-failure")) {
		fail_proc = 1;
		CHECK(logger_fork_reinit() == -1 && errno == EACCES &&
		      !fork_calls);
		fail_proc = 0;
		CHECK(logger_flush_status() == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "fork-failure")) {
		fail_fork = observe_fork = 1;
		old_fd = find_fd("before.log");
		CHECK(logger_fork_reinit() == -1 && errno == EAGAIN &&
		      fork_calls == 1);
		fail_fork = observe_fork = 0;
		old_fd = -1;
		CHECK(logger_process_object_count() == 0);
		CHECK(logger_init(&cfg) == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "io-failure") || !strcmp(mode, "close-failure")) {
		int expected = ENOSPC;
		if (!strcmp(mode, "close-failure")) {
			fail_close = 1;
			old_fd = find_fd("before.log");
			expected = EIO;
		}
		CHECK(logger_fork_reinit() == -1 && errno == expected &&
		      !fork_calls);
		CHECK(logger_process_object_count() == 0);
		old_fd = -1;
		cfg = config("recovered.log", 1);
		CHECK(logger_init(&cfg) == 0);
		one_pair();
		return;
	}
	if (!strcmp(mode, "explicit-recreate")) {
		logger_config_t e = config("explicit-before.log", 1);
		logger_t *before = logger_create(&e);
		CHECK(before != NULL);
		LOGGER_INFO(before, "module", "PRE_EXPLICIT");
		CHECK(logger_flush_instance_status(before) == 0);
		logger_destroy(
			before); /* owner clears its pointer before the boundary */
		before = NULL;
		pid_t child = logger_fork_reinit();
		CHECK(child >= 0);
		e = config(child ? "parent-explicit.log" : "child-explicit.log",
			   1);
		logger_t *after = logger_create(&e);
		CHECK(after != NULL);
		LOGGER_INFO(after, "module", "NEW_EXPLICIT");
		CHECK(logger_flush_instance_status(after) == 0);
		logger_destroy(after);
		if (!child)
			exit(0);
		wait_child(child);
		CHECK(file_contains("child-explicit.log", "NEW_EXPLICIT"));
		CHECK(file_contains("parent-explicit.log", "NEW_EXPLICIT"));
		return;
	}
	if (!strcmp(mode, "audit-conflict")) {
		int pipefd[2];
		CHECK(pipe(pipefd) == 0);
		pid_t child = logger_fork_reinit();
		CHECK(child >= 0);
		audit_config_t a = audit_config("single-chain");
		if (!child) {
			CHECK(close(pipefd[1]) == 0);
			char permit;
			CHECK(read(pipefd[0], &permit, 1) == 1);
			CHECK(close(pipefd[0]) == 0);
			CHECK(audit_init(&a) == -1 && errno == EBUSY);
			audit_one("separate-child");
			exit(0);
		}
		CHECK(close(pipefd[0]) == 0);
		CHECK(audit_init(&a) == 0);
		CHECK(write(pipefd[1], "x", 1) == 1);
		CHECK(close(pipefd[1]) == 0);
		wait_child(child);
		CHECK(audit_shutdown_status() == 0);
		CHECK(audit_verify_file("single-chain.audit.log") == 0);
		return;
	}
	if (!strcmp(mode, "nested")) {
		pid_t child = logger_fork_reinit();
		CHECK(child >= 0);
		if (!child) {
			logger_config_t c = config("nested.log", 1);
			CHECK(logger_init(&c) == 0);
			LOG_INFO("CHILD_BEFORE_NESTED");
			guard_after_raw();
			pid_t grand = logger_fork_reinit();
			CHECK(grand >= 0);
			if (!grand) {
				c = config("grandchild.log", 1);
				CHECK(logger_init(&c) == 0);
				LOG_INFO("GRANDCHILD");
				logger_shutdown();
				exit(0);
			}
			wait_child(grand);
			CHECK(file_contains("grandchild.log", "GRANDCHILD"));
			exit(0);
		}
		wait_child(child);
		CHECK(logger_init(&cfg) == 0);
		logger_shutdown();
		return;
	}
	if (!strcmp(mode, "repeat")) {
		for (unsigned i = 0; i < 10; ++i) {
			one_pair();
			if (i != 9)
				CHECK(logger_init(&cfg) == 0);
		}
		return;
	}
	CHECK(!"unknown mode");
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "reinit-fork-XXXXXX";
	enter_temp(dir);
	run(argv[1]);
	return 0;
}
