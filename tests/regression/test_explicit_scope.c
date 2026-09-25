#define _GNU_SOURCE
#include "global_support.h"
#include "console.h"
#include "audit.h"
#include <dirent.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>

/* All pausing state belongs to this test. The rendezvous has no test mutex
 * which could itself be abandoned by cancellation. Wrappers intercept the
 * real library critical section, not an imitation of the implementation. */
enum point {
	P_NONE,
	P_WRITE,
	P_REOPEN,
	P_QUEUE,
	P_WAIT,
	P_CREATE,
	P_CLOSE,
	P_STDIO,
	P_FORMAT,
	P_LOCK
};
static _Atomic int point, entered, release_call, cancel_disabled;
static _Atomic int hold_worker, worker_entered, release_worker, cleanup_seen;
static _Atomic int returned, create_fail, cancel_setup_fail, stdio_fail,
	flush_fail;
static _Thread_local int actor;
static logger_t *log_instance;
static const char *scenario, *nested;
static char socket_dir[] = "/tmp/logger-scope-XXXXXX";
static char socket_path[108];
static int reentry_mode, cleanup_new, use_console;
static int count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			++n;
	CHECK(!closedir(d));
	return n;
}
static logger_config_t local_cfg(int async)
{
	logger_config_t c = config_for("scope.log", async);
	c.queue_capacity = 2;
	if (!strcmp(scenario, "syslog-metrics")) {
		c.outputs |= LOGGER_OUT_SYSLOG;
		c.syslog.path = socket_path;
	}
	return c;
}
static void nested_call(void)
{
	logger_metrics_t metrics;
	logger_io_metrics_t io;
	logger_syslog_metrics_t sys;
	logger_config_t cfg = config_for("nested.log", 0);
	console_config_t cc = CONSOLE_DEFAULT_CONFIG();
	logger_context_t ctx = { .request_id = "nested" };
	audit_status_t audit;
	errno = 0;
	if (!strcmp(nested, "log"))
		LOGGER_INFO(log_instance, "test", "nested");
	else if (!strcmp(nested, "source"))
		logger_log_source(log_instance, LOGGER_INFO, LOGGER_SOURCE(),
				  "nested");
	else if (!strcmp(nested, "sync"))
		CHECK(logger_log_sync_status(log_instance, LOGGER_INFO, "x",
					     __FILE__, __LINE__, __func__,
					     "nested") == -EDEADLK);
	else if (!strcmp(nested, "flush"))
		CHECK(logger_flush_instance_status(log_instance) == -1);
	else if (!strcmp(nested, "flush-void"))
		logger_flush_instance(log_instance);
	else if (!strcmp(nested, "reopen"))
		CHECK(logger_reopen_instance(log_instance) == -1);
	else if (!strcmp(nested, "destroy"))
		CHECK(logger_destroy_status(log_instance) == -1);
	else if (!strcmp(nested, "destroy-void"))
		logger_destroy(log_instance);
	else if (!strcmp(nested, "create"))
		CHECK(logger_create(&cfg) == NULL);
	else if (!strcmp(nested, "level"))
		logger_set_instance_level(log_instance, LOGGER_OFF);
	else if (!strcmp(nested, "state"))
		CHECK(logger_get_state(log_instance) == LOGGER_STATE_STOPPED);
	else if (!strcmp(nested, "dropped"))
		CHECK(logger_dropped(log_instance) == 0);
	else if (!strcmp(nested, "metrics")) {
		memset(&metrics, 0xff, sizeof(metrics));
		logger_get_metrics(log_instance, &metrics);
		CHECK(!metrics.enqueued);
	} else if (!strcmp(nested, "io")) {
		memset(&io, 0xff, sizeof(io));
		logger_get_io_metrics(log_instance, &io);
		CHECK(!io.emitted_records);
	} else if (!strcmp(nested, "syslog")) {
		memset(&sys, 0x5a, sizeof(sys));
		CHECK(logger_get_syslog_metrics(log_instance, &sys) == -1);
		CHECK(((unsigned char *)&sys)[0] == 0x5a);
	} else if (!strcmp(nested, "context-set"))
		logger_context_set(&ctx);
	else if (!strcmp(nested, "context-clear"))
		logger_context_clear();
	else if (!strcmp(nested, "context-get")) {
		ctx = logger_context_get();
		CHECK(!ctx.request_id);
	} else if (!strcmp(nested, "console"))
		CHECK(console_print("nested") == -1);
	else if (!strcmp(nested, "console-debug"))
		CHECK(console_debug_source(__FILE__, __LINE__, __func__,
					   "nested") == -1);
	else if (!strcmp(nested, "console-init"))
		console_init(&cc);
	else if (!strcmp(nested, "console-level"))
		console_set_verbosity(CONSOLE_QUIET);
	else if (!strcmp(nested, "console-color"))
		console_set_color(CONSOLE_COLOR_ALWAYS);
	else if (!strcmp(nested, "console-tty"))
		CHECK(!console_stdout_is_tty());
	else if (!strcmp(nested, "global"))
		CHECK(logger_shutdown_status() == -1);
	else if (!strcmp(nested, "global-write"))
		LOG_INFO("nested global");
	else if (!strcmp(nested, "audit"))
		CHECK(audit_shutdown_status() == -1);
	else if (!strcmp(nested, "audit-status"))
		CHECK(audit_get_status(&audit) == -1);
	else
		CHECK(!"unknown nested API");
	CHECK(errno == EDEADLK);
	CHECK(access("nested.log", F_OK) == -1 && errno == ENOENT);
}
static void probe(enum point p)
{
	if (!actor)
		return;
	int expected = p;
	if (!atomic_compare_exchange_strong(&point, &expected, P_NONE))
		return;
	if (reentry_mode) {
		nested_call();
		atomic_store(&entered, 1);
		return;
	}
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	atomic_store(&cancel_disabled, old == PTHREAD_CANCEL_DISABLE);
	CHECK(!pthread_setcancelstate(old, NULL));
	atomic_store(&entered, 1);
	wait_flag(&release_call);
}
int __real_logger_file_write(logger_file_t *, const char *, size_t,
			     const struct timespec *, int);
int __wrap_logger_file_write(logger_file_t *f, const char *p, size_t n,
			     const struct timespec *t, int sync)
{
	probe(P_WRITE);
	return __real_logger_file_write(f, p, n, t, sync);
}
int __real_logger_file_reopen(logger_file_t *);
int __wrap_logger_file_reopen(logger_file_t *f)
{
	probe(P_REOPEN);
	return __real_logger_file_reopen(f);
}
int __real_logger_queue_push(logger_queue_t *, const logger_message_t *);
int __wrap_logger_queue_push(logger_queue_t *q, const logger_message_t *m)
{
	probe(P_QUEUE);
	return __real_logger_queue_push(q, m);
}
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
	if (actor && log_instance && mu == &log_instance->progress_mu)
		probe(P_WAIT);
	return __real_pthread_cond_wait(cv, mu);
}
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
			  void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
			  void *(*fn)(void *), void *u)
{
	probe(P_CREATE);
	if (actor && atomic_exchange(&create_fail, 0))
		return EAGAIN;
	return __real_pthread_create(t, a, fn, u);
}
int __real_close(int);
int __wrap_close(int fd)
{
	probe(P_CLOSE);
	return __real_close(fd);
}
int __real_fflush(FILE *);
int __wrap_fflush(FILE *f)
{
	probe(P_STDIO);
	if (actor && atomic_exchange(&flush_fail, 0)) {
		errno = EIO;
		return EOF;
	}
	return __real_fflush(f);
}
int __real_vfprintf(FILE *, const char *, va_list);
int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	if (actor && atomic_exchange(&stdio_fail, 0)) {
		errno = ENOSPC;
		return -1;
	}
	return __real_vfprintf(f, fmt, ap);
}
int __real_vsnprintf(char *, size_t, const char *, va_list);
int __wrap_vsnprintf(char *out, size_t n, const char *fmt, va_list ap)
{
	probe(P_FORMAT);
	return __real_vsnprintf(out, n, fmt, ap);
}
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mu)
{
	int rc = __real_pthread_mutex_lock(mu);
	if (!rc && actor && log_instance && mu == &log_instance->emit_mu)
		probe(P_LOCK);
	return rc;
}
int __real_pthread_setcancelstate(int, int *);
int __wrap_pthread_setcancelstate(int state, int *old)
{
	if (actor && state == PTHREAD_CANCEL_DISABLE &&
	    atomic_exchange(&cancel_setup_fail, 0))
		return EINVAL;
	return __real_pthread_setcancelstate(state, old);
}
size_t __real_logger_format_line(const logger_t *, const logger_message_t *,
				 char *, size_t);
size_t __wrap_logger_format_line(const logger_t *l, const logger_message_t *m,
				 char *out, size_t cap)
{
	if (atomic_exchange(&hold_worker, 0)) {
		atomic_store(&worker_entered, 1);
		wait_flag(&release_worker);
		if (reentry_mode) {
			nested_call();
			atomic_store(&entered, 1);
		}
	}
	return __real_logger_format_line(l, m, out, cap);
}
static void application_cleanup(void *unused)
{
	(void)unused;
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	actor = 0;
	/* Constructor cleanup must have disposed its unreturned candidate first.
     * Destructor caller removed the pointer from its owner before the call. */
	if (cleanup_new) {
		CHECK(logger_process_object_count() == 0);
		logger_config_t cfg = local_cfg(0);
		logger_t *again = logger_create(&cfg);
		CHECK(again);
		LOGGER_INFO(again, "cleanup", "owner-reacquired");
		CHECK(!logger_destroy_status(again));
	} else if (use_console) {
		CHECK(!console_print("cleanup-can-output\n"));
	} else {
		LOGGER_INFO(log_instance, "cleanup", "cleanup-can-log");
		CHECK(!logger_flush_instance_status(log_instance));
	}
	atomic_store(&cleanup_seen, 1);
}
static int console_call(const char *name)
{
	if (!strcmp(name, "print"))
		return console_print("record\n");
	if (!strcmp(name, "info"))
		return console_info("record");
	if (!strcmp(name, "warn"))
		return console_warn("record");
	if (!strcmp(name, "error"))
		return console_error("record");
	if (!strcmp(name, "verbose"))
		return console_verbose("record");
	if (!strcmp(name, "debug"))
		return console_debug("record");
	CHECK(!strcmp(name, "source"));
	return console_debug_source(__FILE__, __LINE__, __func__, "record");
}
static void *caller(void *unused)
{
	(void)unused;
	actor = 1;
	pthread_cleanup_push(application_cleanup, NULL);
	if (use_console)
		CHECK(!console_call(scenario));
	else if (!strcmp(scenario, "create") ||
		 !strcmp(scenario, "create-fail")) {
		logger_config_t cfg = local_cfg(1);
		logger_t *l = logger_create(&cfg);
		/* A pending deferred request must be handled before pointer handoff. */
		if (l) {
			logger_destroy(l);
			CHECK(!"constructor returned despite pending cancellation");
		}
	} else if (!strcmp(scenario, "destroy")) {
		logger_t *owned = log_instance;
		log_instance = NULL;
		CHECK(!logger_destroy_status(owned));
	} else if (!strcmp(scenario, "flush"))
		CHECK(!logger_flush_instance_status(log_instance));
	else if (!strcmp(scenario, "flush-void"))
		logger_flush_instance(log_instance);
	else if (!strcmp(scenario, "reopen"))
		CHECK(!logger_reopen_instance(log_instance));
	else if (!strcmp(scenario, "syslog-metrics")) {
		logger_syslog_metrics_t metrics;
		CHECK(!logger_get_syslog_metrics(log_instance, &metrics));
	} else if (!strcmp(scenario, "source"))
		logger_log_source(log_instance, LOGGER_INFO, LOGGER_SOURCE(),
				  "operation-record");
	else if (!strcmp(scenario, "sync"))
		CHECK(!logger_log_sync_status(log_instance, LOGGER_INFO, "test",
					      __FILE__, __LINE__, __func__,
					      "operation-record"));
	else if (!strcmp(scenario, "fallback"))
		LOGGER_ERROR(log_instance, "test", "operation-record");
	else
		LOGGER_INFO(log_instance, "test", "operation-record");
	atomic_store(&returned, 1);
	pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}
static void cleanup_fixtures(void)
{
	const char *names[] = { "scope.log", "scope.log.logger.lock",
				"nested.log", "nested.log.logger.lock",
				"console.out" };
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
		unlink(names[i]);
}
static void cancel_case(void)
{
	int baseline = count_fds();
	cleanup_new = !strcmp(scenario, "create") ||
		      !strcmp(scenario, "create-fail") ||
		      !strcmp(scenario, "destroy");
	if (!cleanup_new || !strcmp(scenario, "destroy")) {
		int async = !strcmp(scenario, "queue") ||
			    !strcmp(scenario, "flush") ||
			    !strcmp(scenario, "flush-void") ||
			    !strcmp(scenario, "fallback");
		if (!use_console) {
			logger_config_t cfg = local_cfg(async);
			log_instance = logger_create(&cfg);
			CHECK(log_instance);
		}
	}
	if (use_console)
		atomic_store(&point, P_STDIO);
	else if (!strcmp(scenario, "queue"))
		atomic_store(&point, P_QUEUE);
	else if (!strcmp(scenario, "flush") ||
		 !strcmp(scenario, "flush-void") ||
		 !strcmp(scenario, "fallback")) {
		atomic_store(&hold_worker, 1);
		LOGGER_INFO(log_instance, "test", "in-flight-worker");
		wait_flag(&worker_entered);
		if (!strcmp(scenario, "fallback")) {
			LOGGER_INFO(log_instance, "test", "fill1");
			LOGGER_INFO(log_instance, "test", "fill2");
			atomic_store(&point, P_WRITE);
		} else
			atomic_store(&point, P_WAIT);
	} else if (!strcmp(scenario, "reopen"))
		atomic_store(&point, P_REOPEN);
	else if (!strcmp(scenario, "syslog-metrics"))
		atomic_store(&point, P_LOCK);
	else if (!strcmp(scenario, "format"))
		atomic_store(&point, P_FORMAT);
	else if (!strcmp(scenario, "destroy"))
		atomic_store(&point, P_CLOSE);
	else if (!strcmp(scenario, "create") ||
		 !strcmp(scenario, "create-fail")) {
		atomic_store(&point, P_CREATE);
		atomic_store(&create_fail, !strcmp(scenario, "create-fail"));
	} else
		atomic_store(&point, P_WRITE);
	pthread_t t;
	CHECK(!pthread_create(&t, NULL, caller, NULL));
	wait_flag(&entered);
	CHECK(!pthread_cancel(t));
	for (int i = 0; i < 10; ++i)
		nap_ms();
#ifndef LOGGER_SCOPE_BASELINE
	CHECK(atomic_load(&cancel_disabled));
	CHECK(!atomic_load(&cleanup_seen));
#endif
	atomic_store(&release_call, 1);
	atomic_store(&release_worker, 1);
	void *result;
	CHECK(!pthread_join(t, &result));
	CHECK(result == PTHREAD_CANCELED && atomic_load(&cleanup_seen));
	if (log_instance) {
		CHECK(!logger_flush_instance_status(log_instance));
		CHECK(!logger_destroy_status(log_instance));
		log_instance = NULL;
	}
	CHECK(logger_process_object_count() == 0);
	CHECK(count_fds() == baseline);
}
static void reentry_case(const char *origin)
{
	logger_config_t cfg = local_cfg(!strcmp(origin, "worker"));
	log_instance = logger_create(&cfg);
	CHECK(log_instance);
	reentry_mode = 1;
	if (!strcmp(origin, "global-worker")) {
		unlink("global-scope.log");
		logger_config_t gc = config_for("global-scope.log", 1);
		CHECK(!logger_init(&gc));
		atomic_store(&hold_worker, 1);
		atomic_store(&release_worker, 1);
		LOG_INFO("global-worker-outer");
		CHECK(!logger_flush_status());
		LOG_INFO("global-still-usable");
		CHECK(!logger_shutdown_status());
		CHECK(file_contains("global-scope.log", "global-still-usable"));
	} else if (!strcmp(origin, "worker")) {
		atomic_store(&hold_worker, 1);
		atomic_store(&release_worker, 1);
		LOGGER_INFO(log_instance, "test", "outer");
		CHECK(!logger_flush_instance_status(log_instance));
	} else {
		actor = 1;
		if (!strcmp(origin, "create")) {
			logger_config_t other = config_for("create.log", 1);
			unlink("create.log");
			atomic_store(&point, P_CREATE);
			logger_t *l = logger_create(&other);
			CHECK(l);
			CHECK(!logger_destroy_status(l));
		} else if (!strcmp(origin, "destroy")) {
			atomic_store(&point, P_CLOSE);
			CHECK(!logger_destroy_status(log_instance));
			log_instance = logger_create(&cfg);
			CHECK(log_instance);
		} else if (!strcmp(origin, "reopen")) {
			atomic_store(&point, P_REOPEN);
			CHECK(!logger_reopen_instance(log_instance));
		} else if (!strcmp(origin, "console")) {
			atomic_store(&point, P_STDIO);
			CHECK(!console_print("outer\n"));
		} else {
			atomic_store(&point, !strcmp(origin, "format") ?
						     P_FORMAT :
						     P_WRITE);
			LOGGER_INFO(log_instance, "test", "outer");
		}
		actor = 0;
	}
	CHECK(atomic_load(&entered));
	LOGGER_INFO(log_instance, "test", "still-usable");
	CHECK(!logger_destroy_status(log_instance));
	log_instance = NULL;
	CHECK(file_contains("scope.log", "still-usable"));
	CHECK(!logger_process_object_count());
}
static void contract_case(void)
{
	logger_config_t cfg = local_cfg(0);
	log_instance = logger_create(&cfg);
	CHECK(log_instance);
	if (!strcmp(scenario, "restore-policy")) {
		int old, observed, type;
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
		CHECK(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					     &type));
		LOGGER_INFO(log_instance, "test", "disabled");
		CHECK(!logger_flush_instance_status(log_instance));
		CHECK(!console_print("disabled\n"));
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					      &observed));
		CHECK(observed == PTHREAD_CANCEL_DISABLE);
		CHECK(!pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED,
					     &observed));
		CHECK(observed == PTHREAD_CANCEL_ASYNCHRONOUS);
		CHECK(!pthread_setcanceltype(type, NULL));
		CHECK(!pthread_setcancelstate(old, NULL));
		CHECK(!logger_flush_instance_status(log_instance));
		CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
					      &observed));
		CHECK(observed == PTHREAD_CANCEL_ENABLE);
		CHECK(!pthread_setcancelstate(observed, NULL));
	} else if (!strcmp(scenario, "create-async-reject")) {
		logger_config_t other = config_for("nested.log", 0);
		CHECK(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
					     NULL));
		errno = 0;
		logger_t *l = logger_create(&other);
		CHECK(!pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL));
		if (l)
			logger_destroy(l);
		CHECK(l == NULL && errno == ENOTSUP);
		CHECK(access("nested.log", F_OK) != 0);
	} else if (!strcmp(scenario, "setup-failure")) {
		actor = 1;
		atomic_store(&cancel_setup_fail, 1);
		errno = 0;
		CHECK(logger_flush_instance_status(log_instance) == -1 &&
		      errno == EINVAL);
		CHECK(!logger_flush_instance_status(log_instance));
		atomic_store(&cancel_setup_fail, 1);
		errno = 0;
		CHECK(console_print("no-output") == -1 && errno == EINVAL);
		CHECK(!console_print("okay\n"));
		actor = 0;
	} else if (!strcmp(scenario, "stdio-first-error")) {
		actor = 1;
		atomic_store(&stdio_fail, 1);
		atomic_store(&flush_fail, 1);
		errno = 0;
		CHECK(console_info("fail") == -1 && errno == ENOSPC);
		CHECK(!console_info("after-error"));
		actor = 0;
	} else if (!strcmp(scenario, "context-alias")) {
		logger_context_t c = { .request_id = "request",
				       .session_id = "session",
				       .trace_id = "trace" };
		logger_context_set(&c);
		c = logger_context_get();
		logger_context_set(&c);
		c = logger_context_get();
		CHECK(!strcmp(c.request_id, "request"));
		const char *swap = c.request_id;
		c.request_id = c.session_id;
		c.session_id = swap;
		logger_context_set(&c);
		c = logger_context_get();
		CHECK(!strcmp(c.request_id, "session") &&
		      !strcmp(c.session_id, "request"));
		logger_context_clear();
		CHECK(!logger_context_get().request_id);
	} else if (!strcmp(scenario, "debug-overflow")) {
		char s[4000];
		memset(s, 'x', sizeof(s) - 1);
		s[sizeof(s) - 1] = 0;
		errno = 0;
		CHECK(console_debug_source(__FILE__, 1, __func__, "%s", s) ==
			      -1 &&
		      errno == EOVERFLOW);
		CHECK(!console_debug_source(__FILE__, 1, __func__, "short"));
	} else if (!strcmp(scenario, "invalid-level")) {
		errno = 0;
		logger_set_instance_level(log_instance, (logger_level_t)-1);
		CHECK(errno == EINVAL);
		LOGGER_INFO(log_instance, "test", "level-unchanged");
		CHECK(!logger_flush_instance_status(log_instance));
		CHECK(file_contains("scope.log", "level-unchanged"));
	} else
		CHECK(!"unknown contract scenario");
	CHECK(!logger_destroy_status(log_instance));
	log_instance = NULL;
}
int main(int argc, char **argv)
{
	CHECK(argc >= 3);
	scenario = argv[2];
	cleanup_fixtures();
	int receiver = -1;
	if (!strcmp(scenario, "syslog-metrics")) {
		CHECK(mkdtemp(socket_dir));
		CHECK(snprintf(socket_path, sizeof(socket_path), "%s/log",
			       socket_dir) > 0);
		receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		CHECK(receiver >= 0);
		struct sockaddr_un address = { .sun_family = AF_UNIX };
		strcpy(address.sun_path, socket_path);
		CHECK(!bind(receiver, (struct sockaddr *)&address,
			    sizeof(address)));
	}
	/* Isolate stdio output. Preserve stderr for CHECK diagnostics after tests. */
	FILE *sink = fopen("console.out", "w+");
	CHECK(sink);
	FILE *saved_out = stdout, *saved_err = stderr;
	stdout = sink;
	stderr = sink;
	console_config_t cc = CONSOLE_DEFAULT_CONFIG();
	cc.verbosity = CONSOLE_DEBUG;
	cc.color = CONSOLE_COLOR_NEVER;
	console_init(&cc);
	if (!strcmp(argv[1], "cancel")) {
		cancel_case();
	} else if (!strcmp(argv[1], "console-cancel")) {
		use_console = 1;
		cancel_case();
	} else if (!strcmp(argv[1], "reentry")) {
		CHECK(argc == 4);
		nested = argv[3];
		reentry_case(argv[2]);
	} else {
		CHECK(!strcmp(argv[1], "contract"));
		contract_case();
	}
	stdout = saved_out;
	stderr = saved_err;
	CHECK(!fclose(sink));
	if (receiver >= 0) {
		CHECK(!close(receiver));
		CHECK(!unlink(socket_path));
		CHECK(!rmdir(socket_dir));
	}
	puts("explicit/console scope regression passed");
	return 0;
}
