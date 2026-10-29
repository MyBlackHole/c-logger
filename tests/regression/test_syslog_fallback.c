#define _GNU_SOURCE
#include "support.h"
#include "logger_internal.h"
#include <sys/socket.h>
#include <sys/un.h>
static _Atomic int hold_worker;
static _Atomic int entered;
size_t __real_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *buf,
				 size_t n);
size_t __wrap_logger_format_line(const logger_t *l,
				 const struct logger_message *m, char *buf,
				 size_t n)
{
	if (atomic_load(&hold_worker) && !strcmp(m->text, "held")) {
		atomic_store(&entered, 1);
		while (atomic_load(&hold_worker))
			nap_ms();
	}
	return __real_logger_format_line(l, m, buf, n);
}
int main(void)
{
	CHECK(logger_process_init() == 0);
	char dir[] = "/tmp/lg-sys-fb-XXXXXX";
	char path[108];
	CHECK(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/sink", dir);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, path);
	CHECK(!bind(fd, (struct sockaddr *)&a, sizeof(a)));
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_SYSLOG;
	c.syslog.path = path;
	c.queue_capacity = 2;
	c.overflow[LOGGER_INFO] = LOGGER_OVERFLOW_SYNC;
	logger_t *l = NULL;
	CHECK(logger_create(&c, &l) == 0);
	CHECK(l != NULL);
	int rc = 0;
	unsigned int tries = 0;
	errno = EDOM;
	do {
		rc = logger_log_sync_status(l, LOGGER_INFO, "fill", NULL, 0,
					    NULL, "fill");
		CHECK(errno == EDOM);
		CHECK(++tries < 10000);
	} while (!rc);
	CHECK(rc == -EAGAIN);
	atomic_store(&hold_worker, 1);
	CHECK(LOGGER_INFO(l, "q", "held") == 0 && errno == EDOM);
	wait_flag(&entered);
	errno = EDOM;
	CHECK(LOGGER_INFO(l, "q", "queue-a") == 0 && errno == EDOM);
	CHECK(LOGGER_INFO(l, "q", "queue-b") == 0 && errno == EDOM);
	CHECK(LOGGER_INFO(l, "q", "sync-fallback") == -EAGAIN && errno == EDOM);
	logger_metrics_t m;
	CHECK(logger_get_metrics(l, &m) == 0);
	uint64_t dropped;
	CHECK(logger_dropped(l, &dropped) == 0 && dropped == 0);
	CHECK(m.enqueued == 3 && m.sync_fallbacks == 1);
	logger_syslog_metrics_t io;
	CHECK(!logger_get_syslog_metrics(l, &io));
	CHECK(io.failed_records == 2);
	atomic_store(&hold_worker, 0);
	CHECK(logger_flush(l) == -EAGAIN);
	CHECK(!logger_get_syslog_metrics(l, &io));
	CHECK(io.failed_records == 5 && io.backpressure_records == 5);
	logger_io_metrics_t all;
	CHECK(logger_get_io_metrics(l, &all) == 0);
	CHECK(all.async_completed == 3);
	CHECK(logger_destroy(&l) == -EAGAIN);
	CHECK(!close(fd));
	CHECK(!unlink(path));
	CHECK(!rmdir(dir));
	puts("syslog queue-full sync fallback is bounded and accounted");
	return 0;
}
