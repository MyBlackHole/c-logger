#define _GNU_SOURCE
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { 	fprintf(stderr, "CHECK failed: %s (%s:%d) errno=%d\n", #x, __FILE__, __LINE__, errno); 	exit(2); } } while (0)

typedef struct {
	char dir[256];
	char socket_path[320];
	char socket_dir[320];
	char output_path[320];
	char config_path[320];
	char pid_path[320];
	char work_path[320];
	pid_t daemon_pid;
} fixture_t;

static void sleep_ms(unsigned ms)
{
	struct timespec ts = {
		.tv_sec = (time_t)(ms / 1000u),
		.tv_nsec = (long)(ms % 1000u) * 1000000L
	};
	while (nanosleep(&ts, &ts) != 0)
		CHECK(errno == EINTR);
}

static void write_config(const fixture_t *f)
{
	FILE *out = fopen(f->config_path, "w");
	CHECK(out != NULL);
	CHECK(fprintf(out,
		      "global(workDirectory=\"%s\")\n"
		      "module(load=\"imuxsock\" SysSock.Use=\"off\")\n"
		      "input(type=\"imuxsock\" Socket=\"%s\" CreatePath=\"on\" RateLimit.Interval=\"0\")\n"
		      "template(name=\"raw\" type=\"string\" string=\"%%rawmsg%%\\n\")\n"
		      "action(type=\"omfile\" file=\"%s\" template=\"raw\" flushOnTXEnd=\"on\")\n",
		      f->work_path, f->socket_path, f->output_path) > 0);
	CHECK(fclose(out) == 0);
}

static int socket_ready(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static void start_daemon(fixture_t *f)
{
	CHECK(f->daemon_pid <= 0);
	(void)unlink(f->socket_path);
	(void)unlink(f->pid_path);
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		execlp("rsyslogd", "rsyslogd", "-n", "-f", f->config_path,
		       "-i", f->pid_path, (char *)NULL);
		_exit(127);
	}
	f->daemon_pid = child;
	for (unsigned i = 0; i < 500; ++i) {
		int status;
		pid_t rc = waitpid(child, &status, WNOHANG);
		CHECK(rc >= 0);
		if (rc == child) {
			fprintf(stderr, "rsyslogd exited early status=%d\n", status);
			exit(2);
		}
		if (socket_ready(f->socket_path))
			return;
		sleep_ms(10);
	}
	fprintf(stderr, "rsyslogd socket did not appear\n");
	exit(2);
}

static void stop_daemon(fixture_t *f)
{
	if (f->daemon_pid <= 0)
		return;
	CHECK(kill(f->daemon_pid, SIGCONT) == 0 || errno == ESRCH);
	CHECK(kill(f->daemon_pid, SIGTERM) == 0 || errno == ESRCH);
	int status;
	CHECK(waitpid(f->daemon_pid, &status, 0) == f->daemon_pid);
	CHECK(WIFEXITED(status) || WIFSIGNALED(status));
	f->daemon_pid = -1;
	for (unsigned i = 0; i < 100 && socket_ready(f->socket_path); ++i)
		sleep_ms(10);
	(void)unlink(f->socket_path);
	(void)unlink(f->pid_path);
}

static int file_contains(const char *path, const char *needle)
{
	FILE *in = fopen(path, "rb");
	if (!in)
		return 0;
	char buffer[8192];
	size_t n = fread(buffer, 1, sizeof(buffer) - 1u, in);
	CHECK(!ferror(in));
	CHECK(fclose(in) == 0);
	buffer[n] = '\0';
	return strstr(buffer, needle) != NULL;
}

static void wait_contains(const fixture_t *f, const char *needle)
{
	for (unsigned i = 0; i < 300; ++i) {
		if (file_contains(f->output_path, needle))
			return;
		sleep_ms(10);
	}
	fprintf(stderr, "rsyslog output missing marker: %s\n", needle);
	exit(2);
}

static logger_config_t syslog_config(const fixture_t *f,
				     logger_syslog_start_t startup)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_SYSLOG;
	c.async_mode = 0;
	c.level = LOGGER_TRACE;
	c.ident = "real-syslogd";
	c.syslog.path = f->socket_path;
	c.syslog.facility = LOGGER_SYSLOG_FACILITY_LOCAL0;
	c.syslog.reconnect_interval_ms = 100;
	c.syslog.startup = startup;
	return c;
}

static int write_one(logger_t *log, const char *text)
{
	return logger_log_sync_status(log, LOGGER_INFO, "integration",
				      __FILE__, __LINE__, __func__, "%s", text);
}

static logger_syslog_metrics_t metrics(logger_t *log)
{
	logger_syslog_metrics_t out;
	CHECK(logger_get_syslog_metrics(log, &out) == 0);
	CHECK(out.submitted_records == out.sent_records + out.failed_records);
	return out;
}

static logger_diagnostics_t diagnostics(logger_t *log)
{
	logger_diagnostics_t out;
	memset(&out, 0, sizeof(out));
	logger_get_diagnostics(log, &out);
	return out;
}

static void scenario_required_missing(fixture_t *f)
{
	stop_daemon(f);
	(void)unlink(f->socket_path);
	logger_config_t c = syslog_config(f, LOGGER_SYSLOG_START_REQUIRED);
	errno = 0;
	logger_t *log = logger_create(&c);
	CHECK(log == NULL);
	CHECK(errno == ENOENT || errno == ECONNREFUSED);
}

static void scenario_required_delivery(fixture_t *f)
{
	start_daemon(f);
	logger_config_t c = syslog_config(f, LOGGER_SYSLOG_START_REQUIRED);
	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	CHECK(write_one(log, "required-delivery") == 0);
	wait_contains(f, "required-delivery");
	logger_syslog_metrics_t m = metrics(log);
	CHECK(m.connected);
	CHECK(m.connect_attempts == 1);
	CHECK(m.connect_failures == 0);
	CHECK(m.sent_records == 1);
	CHECK(m.failed_records == 0);
	CHECK(logger_flush_instance_status(log) == 0);
	CHECK(logger_destroy_status(log) == 0);
	stop_daemon(f);
}

static void scenario_restart_reconnect(fixture_t *f)
{
	start_daemon(f);
	logger_config_t c = syslog_config(f, LOGGER_SYSLOG_START_REQUIRED);
	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	CHECK(write_one(log, "before-restart") == 0);
	wait_contains(f, "before-restart");

	stop_daemon(f);
	int lost = write_one(log, "lost-not-replayed");
	CHECK(lost < 0);
	logger_syslog_metrics_t m = metrics(log);
	CHECK(!m.connected);
	CHECK(m.failed_records == 1);
	unsigned attempts = (unsigned)m.connect_attempts;

	int cooldown = write_one(log, "cooldown-record");
	CHECK(cooldown < 0);
	m = metrics(log);
	CHECK(m.cooldown_records >= 1);
	CHECK(m.connect_attempts == attempts);

	logger_diagnostics_t d = diagnostics(log);
	CHECK(d.first_error != 0);
	CHECK(d.observation_flags & LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED);

	start_daemon(f);
	sleep_ms(150);
	CHECK(write_one(log, "after-restart") == 0);
	wait_contains(f, "after-restart");
	CHECK(!file_contains(f->output_path, "lost-not-replayed"));
	m = metrics(log);
	CHECK(m.connected);
	CHECK(m.reconnect_successes == 1);
	CHECK(m.connect_attempts == attempts + 1u);
	CHECK(m.last_error == 0);

	errno = 0;
	CHECK(logger_flush_instance_status(log) == -1);
	CHECK(errno != 0);
	errno = 0;
	CHECK(logger_destroy_status(log) == -1);
	CHECK(errno != 0);
	stop_daemon(f);
}

static void scenario_deferred_reconnect(fixture_t *f)
{
	stop_daemon(f);
	(void)unlink(f->socket_path);
	logger_config_t c = syslog_config(f, LOGGER_SYSLOG_START_DEFERRED);
	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	logger_syslog_metrics_t m = metrics(log);
	CHECK(!m.connected);
	CHECK(m.connect_attempts == 1);
	CHECK(m.connect_failures == 1);
	CHECK(m.last_error != 0);

	CHECK(write_one(log, "deferred-cooldown") < 0);
	m = metrics(log);
	CHECK(m.cooldown_records >= 1);
	unsigned attempts = (unsigned)m.connect_attempts;

	start_daemon(f);
	sleep_ms(150);
	CHECK(write_one(log, "deferred-recovered") == 0);
	wait_contains(f, "deferred-recovered");
	m = metrics(log);
	CHECK(m.connected);
	CHECK(m.reconnect_successes == 1);
	CHECK(m.connect_attempts == attempts + 1u);
	CHECK(m.last_error == 0);

	errno = 0;
	CHECK(logger_destroy_status(log) == -1);
	CHECK(errno != 0);
	stop_daemon(f);
}

static void scenario_real_backpressure(fixture_t *f)
{
	start_daemon(f);
	logger_config_t c = syslog_config(f, LOGGER_SYSLOG_START_REQUIRED);
	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	CHECK(write_one(log, "before-backpressure") == 0);
	wait_contains(f, "before-backpressure");

	CHECK(kill(f->daemon_pid, SIGSTOP) == 0);
	char payload[4001];
	memset(payload, 'x', sizeof(payload) - 1u);
	payload[sizeof(payload) - 1u] = '\0';
	int observed = 0;
	for (unsigned i = 0; i < 20000; ++i) {
		int rc = write_one(log, payload);
		if (rc == -EAGAIN || rc == -ENOBUFS || rc == -ENOMEM) {
			observed = 1;
			break;
		}
		CHECK(rc == 0);
	}
	CHECK(observed);
	logger_syslog_metrics_t m = metrics(log);
	CHECK(m.backpressure_records >= 1);
	CHECK(m.failed_records >= 1);
	CHECK(m.connected);
	logger_diagnostics_t d = diagnostics(log);
	CHECK(d.observation_flags & LOGGER_DIAG_OUTPUT_FAILURE_OBSERVED);

	CHECK(kill(f->daemon_pid, SIGCONT) == 0);
	sleep_ms(50);
	errno = 0;
	CHECK(logger_flush_instance_status(log) == -1);
	CHECK(errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM);
	errno = 0;
	CHECK(logger_destroy_status(log) == -1);
	CHECK(errno == EAGAIN || errno == ENOBUFS || errno == ENOMEM);
	stop_daemon(f);
}

int main(void)
{
	fixture_t f = { .daemon_pid = -1 };
	char temp[] = "/tmp/c-logger-rsyslog-XXXXXX";
	CHECK(mkdtemp(temp) != NULL);
	CHECK(snprintf(f.dir, sizeof(f.dir), "%s", temp) > 0);
	CHECK(snprintf(f.socket_dir, sizeof(f.socket_dir), "%s/dev", temp) > 0);
	CHECK(snprintf(f.socket_path, sizeof(f.socket_path), "%s/log", f.socket_dir) > 0);
	CHECK(snprintf(f.output_path, sizeof(f.output_path), "%s/received.log", temp) > 0);
	CHECK(snprintf(f.config_path, sizeof(f.config_path), "%s/rsyslog.conf", temp) > 0);
	CHECK(snprintf(f.pid_path, sizeof(f.pid_path), "%s/rsyslog.pid", temp) > 0);
	CHECK(snprintf(f.work_path, sizeof(f.work_path), "%s/work", temp) > 0);
	CHECK(mkdir(f.work_path, 0700) == 0);
	write_config(&f);

	scenario_required_missing(&f);
	scenario_required_delivery(&f);
	scenario_restart_reconnect(&f);
	scenario_deferred_reconnect(&f);
	scenario_real_backpressure(&f);

	stop_daemon(&f);
	(void)unlink(f.socket_path);
	CHECK(rmdir(f.socket_dir) == 0);
	(void)unlink(f.output_path);
	(void)unlink(f.config_path);
	(void)unlink(f.pid_path);
	CHECK(rmdir(f.work_path) == 0);
	CHECK(rmdir(f.dir) == 0);
	puts("real rsyslogd integration passed");
	return 0;
}
