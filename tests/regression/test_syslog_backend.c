#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static char temp[] = "/tmp/lg-sys-XXXXXX";
static char endpoint[108];

static int receiver(const char *path, int type)
{
	int fd = socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	CHECK(strlen(path) < sizeof(a.sun_path));
	strcpy(a.sun_path, path);
	CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	if (type == SOCK_STREAM)
		CHECK(listen(fd, 8) == 0);
	return fd;
}
static logger_config_t config(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_SYSLOG;
	c.async_mode = 0;
	c.ident = "unit";
	c.syslog.path = endpoint;
	c.syslog.reconnect_interval_ms = 1;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.file_path = "out.log";
	c.level = LOGGER_TRACE;
	return c;
}
static int write_one(logger_t *l, const char *text)
{
	return logger_log_sync_status(l, LOGGER_INFO, "test", __FILE__,
				      __LINE__, __func__, "%s", text);
}
static logger_syslog_metrics_t metrics(logger_t *l)
{
	logger_syslog_metrics_t m;
	CHECK(logger_get_syslog_metrics(l, &m) == 0);
	CHECK(m.submitted_records == m.sent_records + m.failed_records);
	return m;
}
static void read_one(int fd, char *buf, size_t cap)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	CHECK(poll(&p, 1, 2000) == 1);
	ssize_t n = recv(fd, buf, cap - 1, MSG_DONTWAIT);
	CHECK(n > 0);
	buf[n] = 0;
}
static unsigned drain(int fd)
{
	unsigned n = 0;
	char b[10000];
	for (;;) {
		ssize_t rc = recv(fd, b, sizeof(b), MSG_DONTWAIT);
		if (rc >= 0) {
			++n;
			continue;
		}
		CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
		return n;
	}
}
static unsigned fill(logger_t *l)
{
	for (unsigned i = 0; i < 10000; ++i) {
		int rc = write_one(l, "fill");
		if (rc) {
			CHECK(rc == -EAGAIN || rc == -EWOULDBLOCK);
			return i;
		}
	}
	CHECK(!"receiver did not fill");
	return 0;
}
static unsigned fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	unsigned n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			++n;
	CHECK(!closedir(d));
	return n;
}
static void check_file(unsigned total, int queued)
{
	FILE *f = fopen("out.log", "r");
	CHECK(f);
	char *line = NULL;
	size_t cap = 0;
	unsigned count = 0, seen_count = 0;
	unsigned char seen[4000] = { 0 };
	while (getline(&line, &cap, f) >= 0) {
		++count;
		char *mark = strstr(line, " queued-");
		if (mark) {
			unsigned i;
			CHECK(sscanf(mark, " queued-%u", &i) == 1 && i < 4000 &&
			      !seen[i]);
			seen[i] = 1;
			++seen_count;
		}
	}
	CHECK(!queued || seen_count == 4000);
	CHECK(!ferror(f));
	free(line);
	CHECK(!fclose(f));
	CHECK(count == total);
}
static void pause_reconnect(void)
{
	struct timespec ts = { 0, 5000000 };
	while (nanosleep(&ts, &ts))
		CHECK(errno == EINTR);
}

struct thread_arg {
	logger_t *l;
	unsigned n;
};
static void *writer(void *p)
{
	struct thread_arg *a = p;
	for (unsigned i = 0; i < a->n; ++i)
		LOGGER_INFO(a->l, "threads", "message=%u", i);
	return NULL;
}
static _Atomic int observe;
static void *observer(void *p)
{
	logger_t *l = p;
	while (atomic_load(&observe))
		(void)metrics(l);
	return NULL;
}

static void run(const char *mode)
{
	logger_config_t c = config();
	char packet[10000];
	if (!strcmp(mode, "invalid-metrics")) {
		logger_syslog_metrics_t before, after;
		memset(&before, 0xa5, sizeof(before));
		after = before;
		CHECK(logger_get_syslog_metrics(NULL, &after) == -1 &&
		      errno == EINVAL);
		CHECK(!memcmp(&before, &after, sizeof(after)));
		c.outputs = LOGGER_OUT_FILE;
		logger_t *l = logger_create(&c);
		CHECK(l);
		CHECK(logger_get_syslog_metrics(l, &after) == -1 &&
		      errno == ENOTSUP);
		CHECK(!memcmp(&before, &after, sizeof(after)));
		CHECK(logger_get_syslog_metrics(l, NULL) == -1 &&
		      errno == EINVAL);
		CHECK(logger_destroy_status(l) == 0);
		return;
	}
	if (!strcmp(mode, "required-missing") || !strcmp(mode, "fd-churn") ||
	    !strcmp(mode, "init-cleanup")) {
		unsigned before = fd_count();
		if (!strcmp(mode, "init-cleanup"))
			c.outputs |= LOGGER_OUT_FILE;
		for (unsigned i = 0; i < 64; ++i) {
			CHECK(!logger_create(&c));
			CHECK(errno == ENOENT);
		}
		CHECK(fd_count() == before);
		if (c.outputs & LOGGER_OUT_FILE) {
			c.outputs = LOGGER_OUT_FILE;
			logger_t *l = logger_create(&c);
			CHECK(l);
			CHECK(!logger_destroy_status(l));
		}
		return;
	}
	if (!strcmp(mode, "deferred")) {
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		logger_t *l = logger_create(&c);
		CHECK(l);
		logger_syslog_metrics_t m = metrics(l);
		CHECK(!m.connected && m.connect_failures == 1 &&
		      !m.submitted_records && m.last_error == ENOENT);
		CHECK(logger_flush_instance_status(l) == -1 && errno == ENOENT);
		int fd = receiver(endpoint, SOCK_DGRAM);
		pause_reconnect();
		CHECK(!write_one(l, "first-deliverable"));
		read_one(fd, packet, sizeof(packet));
		CHECK(strstr(packet, "first-deliverable"));
		CHECK(!drain(fd));
		m = metrics(l);
		CHECK(m.connected && m.last_error == 0 && m.sent_records == 1 &&
		      m.reconnect_successes == 1);
		CHECK(logger_destroy_status(l) == -1 && errno == ENOENT);
		CHECK(!close(fd));
		return;
	}
	if (!strcmp(mode, "endpoint-type")) {
		int fd = receiver(endpoint, SOCK_STREAM);
		c.syslog.startup = LOGGER_SYSLOG_START_DEFERRED;
		CHECK(!logger_create(&c));
		CHECK(errno == EPROTOTYPE);
		CHECK(!close(fd));
		return;
	}
	if (!strcmp(mode, "bad-facility") || !strcmp(mode, "bad-startup") ||
	    !strcmp(mode, "bad-interval") || !strcmp(mode, "empty-path") ||
	    !strcmp(mode, "long-path") || !strcmp(mode, "long-tag") ||
	    !strcmp(mode, "tag-injection")) {
		char long_s[300];
		memset(long_s, 'a', sizeof(long_s) - 1);
		long_s[sizeof(long_s) - 1] = 0;
		int expected = EINVAL;
		if (!strcmp(mode, "bad-facility"))
			c.syslog.facility = 15;
		if (!strcmp(mode, "bad-startup"))
			c.syslog.startup = (logger_syslog_start_t)9;
		if (!strcmp(mode, "bad-interval"))
			c.syslog.reconnect_interval_ms = 60001;
		if (!strcmp(mode, "empty-path"))
			c.syslog.path = "";
		if (!strcmp(mode, "long-path")) {
			c.syslog.path = long_s;
			expected = ENAMETOOLONG;
		}
		if (!strcmp(mode, "long-tag")) {
			c.ident = long_s;
			expected = ENAMETOOLONG;
		}
		if (!strcmp(mode, "tag-injection"))
			c.ident = "tag\nforged:";
		CHECK(!logger_create(&c));
		CHECK(errno == expected);
		return;
	}
	int fd = receiver(endpoint, SOCK_DGRAM);
	char copied_path[108];
	strcpy(copied_path, endpoint);
	char copied_tag[128] = "snapshot";
	if (!strcmp(mode, "copy-path")) {
		c.syslog.path = copied_path;
		c.ident = copied_tag;
	}
	if (!strcmp(mode, "cwd"))
		c.syslog.path = "sink";
	if (!strcmp(mode, "default-tag"))
		c.ident = NULL;
	if (!strcmp(mode, "max-tag")) {
		memset(copied_tag, 'x', 127);
		copied_tag[127] = 0;
		c.ident = copied_tag;
	}
	if (!strcmp(mode, "wire"))
		c.syslog.facility = 21u * 8u;
	if (!strcmp(mode, "multi-backend") || !strcmp(mode, "async-pressure"))
		c.outputs |= LOGGER_OUT_FILE;
	if (!strcmp(mode, "async-pressure")) {
		c.async_mode = 1;
		c.queue_capacity = 8192;
	}
	logger_t *l = logger_create(&c);
	CHECK(l);
	if (!strcmp(mode, "file-fails")) {
		/* An independent instance with a failing file still attempts Syslog. */
		CHECK(!logger_destroy_status(l));
		c.outputs |= LOGGER_OUT_FILE;
		c.file_path = "/dev/full";
		l = logger_create(&c);
		CHECK(l);
		CHECK(write_one(l, "syslog-despite-file-error") == -ENOSPC);
		read_one(fd, packet, sizeof(packet));
		CHECK(strstr(packet, "syslog-despite-file-error"));
		CHECK(metrics(l).sent_records == 1 &&
		      metrics(l).failed_records == 0);
		CHECK(logger_destroy_status(l) == -1 && errno == ENOSPC);
		CHECK(!close(fd));
		return;
	} else if (!strcmp(mode, "flags")) {
		CHECK(fcntl(l->syslog_backend.fd, F_GETFL) & O_NONBLOCK);
		CHECK(fcntl(l->syslog_backend.fd, F_GETFD) & FD_CLOEXEC);
		CHECK(metrics(l).connect_attempts == 1);
	} else if (!strcmp(mode, "wire")) {
		const unsigned pri[] = { 7, 7, 6, 4, 3, 2 };
		for (unsigned i = 0; i < 6; ++i) {
			CHECK(!logger_log_sync_status(l, (logger_level_t)i, "m",
						      NULL, 0, NULL,
						      "record-%u", i));
			read_one(fd, packet, sizeof(packet));
			char head[30];
			snprintf(head, sizeof(head), "<%u>unit[",
				 21u * 8u + pri[i]);
			CHECK(strstr(packet, head) == packet);
			CHECK(packet[strlen(packet) - 1] != '\n');
		}
		CHECK(metrics(l).sent_records == 6);
	} else if (!strcmp(mode, "default-tag") || !strcmp(mode, "max-tag")) {
		CHECK(!write_one(l, "tag-case"));
		read_one(fd, packet, sizeof(packet));
		CHECK(strstr(packet, c.ident ? c.ident : "app"));
	} else if (!strcmp(mode, "pressure") ||
		   !strcmp(mode, "multi-backend") ||
		   !strcmp(mode, "async-pressure")) {
		unsigned accepted = fill(l);
		CHECK(accepted > 0);
		if (c.async_mode) {
			for (unsigned i = 0; i < 4000; ++i)
				LOGGER_INFO(l, "async", "queued-%u", i);
		} else {
			for (unsigned i = 0; i < 32; ++i)
				CHECK(write_one(l, "failed-no-replay") ==
				      -EAGAIN);
		}
		CHECK(logger_flush_instance_status(l) == -1 && errno == EAGAIN);
		logger_syslog_metrics_t m = metrics(l);
		unsigned extra = c.async_mode ? 4000 : 32;
		CHECK(m.submitted_records == accepted + 1 + extra);
		CHECK(m.sent_records == accepted &&
		      m.failed_records == 1 + extra &&
		      m.backpressure_records == m.failed_records);
		CHECK(m.connected && m.connect_attempts == 1);
		logger_io_metrics_t io;
		logger_get_io_metrics(l, &io);
		CHECK(io.emitted_records == accepted &&
		      io.failed_records == 1 + extra);
		CHECK(logger_dropped(l) == 0);
		if (c.async_mode)
			CHECK(io.async_completed == 4000);
		CHECK(drain(fd) == accepted);
		CHECK(!write_one(l, "after-pressure"));
		read_one(fd, packet, sizeof(packet));
		CHECK(strstr(packet, "after-pressure"));
		CHECK(!drain(fd));
		m = metrics(l);
		CHECK(m.last_error == 0 && m.connect_attempts == 1);
		if (c.outputs & LOGGER_OUT_FILE) {
			CHECK(logger_flush_instance_status(l) == -1 &&
			      errno == EAGAIN);
			check_file(accepted + extra + 2, c.async_mode);
		}
		CHECK(logger_destroy_status(l) == -1 && errno == EAGAIN);
		CHECK(!close(fd));
		return;
	} else if (!strcmp(mode, "reconnect") || !strcmp(mode, "copy-path") ||
		   !strcmp(mode, "cwd")) {
		CHECK(!write_one(l, "before"));
		read_one(fd, packet, sizeof(packet));
		if (!strcmp(mode, "copy-path")) {
			strcpy(copied_path, "/definitely/not/used");
			strcpy(copied_tag, "mutated");
		}
		if (!strcmp(mode, "cwd"))
			CHECK(!chdir("/"));
		CHECK(!close(fd));
		CHECK(!unlink(endpoint));
		CHECK(write_one(l, "lost-not-replayed") < 0);
		CHECK(!metrics(l).connected);
		fd = receiver(endpoint, SOCK_DGRAM);
		pause_reconnect();
		CHECK(!write_one(l, "after-restart"));
		read_one(fd, packet, sizeof(packet));
		CHECK(strstr(packet, "after-restart") &&
		      !strstr(packet, "lost-not-replayed"));
		if (!strcmp(mode, "copy-path"))
			CHECK(strstr(packet, "snapshot[") &&
			      !strstr(packet, "mutated"));
		CHECK(!drain(fd));
		CHECK(metrics(l).reconnect_successes == 1);
		CHECK(logger_destroy_status(l) == -1);
		CHECK(!close(fd));
		return;
	} else if (!strcmp(mode, "isolation")) {
		char other[108];
		snprintf(other, sizeof(other), "%s/other", temp);
		int f2 = receiver(other, SOCK_DGRAM);
		logger_config_t c2 = config();
		c2.syslog.path = other;
		c2.ident = "second";
		logger_t *l2 = logger_create(&c2);
		CHECK(l2);
		CHECK(fill(l) > 0);
		CHECK(!write_one(l2, "independent"));
		read_one(f2, packet, sizeof(packet));
		CHECK(strstr(packet, "second[") &&
		      strstr(packet, "independent"));
		CHECK(logger_destroy_status(l) == -1);
		l = NULL;
		CHECK(!write_one(l2, "still-alive"));
		read_one(f2, packet, sizeof(packet));
		CHECK(strstr(packet, "still-alive"));
		CHECK(!logger_destroy_status(l2));
		CHECK(!close(f2));
		CHECK(!unlink(other));
	} else if (!strcmp(mode, "threads")) {
		struct thread_arg a = { l, 2000 };
		pthread_t t[8], obs;
		atomic_store(&observe, 1);
		CHECK(!pthread_create(&obs, NULL, observer, l));
		for (unsigned i = 0; i < 8; ++i)
			CHECK(!pthread_create(&t[i], NULL, writer, &a));
		for (unsigned i = 0; i < 8; ++i)
			CHECK(!pthread_join(t[i], NULL));
		atomic_store(&observe, 0);
		CHECK(!pthread_join(obs, NULL));
		CHECK(metrics(l).submitted_records == 16000);
		CHECK(logger_destroy_status(l) == -1 && errno == EAGAIN);
		l = NULL;
	} else if (!strcmp(mode, "fork-guard")) {
		pid_t child = fork();
		CHECK(child >= 0);
		if (!child) {
			logger_syslog_metrics_t out, before;
			memset(&before, 0xa5, sizeof(before));
			out = before;
			int rc = logger_get_syslog_metrics(l, &out);
			_exit(rc == -1 && errno == ECHILD &&
					      !memcmp(&out, &before,
						      sizeof(out)) ?
				      0 :
				      2);
		}
		int st;
		CHECK(waitpid(child, &st, 0) == child && WIFEXITED(st) &&
		      WEXITSTATUS(st) == 0);
		CHECK(!write_one(l, "parent-alive"));
		read_one(fd, packet, sizeof(packet));
	} else {
		CHECK(!"unknown scenario");
	}
	if (l)
		CHECK(!logger_destroy_status(l));
	CHECK(!close(fd));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	CHECK(mkdtemp(temp));
	CHECK(!chdir(temp));
	CHECK(snprintf(endpoint, sizeof(endpoint), "%s/sink", temp) > 0);
	run(argv[1]);
	CHECK(!chdir(temp));
	unlink("sink");
	unlink("out.log");
	unlink("out.log.logger.lock");
	CHECK(!chdir("/"));
	CHECK(!rmdir(temp));
	printf("syslog public scenario passed: %s\n", argv[1]);
	return 0;
}
