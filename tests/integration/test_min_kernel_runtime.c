#define _GNU_SOURCE
#include <audit.h>
#include <logger.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "CHECK failed: %s line=%d errno=%d\n", \
				#expr, __LINE__, errno);                       \
			return -1;                                         \
		}                                                          \
	} while (0)

static int rotation_check(const char *path)
{
	(void)unlink(path);
	char lock[4096];
	CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", path) > 0);
	(void)unlink(lock);

	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1;
	c.rotation.retention_days = 0;

	logger_t *log = logger_create(&c);
	CHECK(log != NULL);
	CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
				     __FILE__, __LINE__, __func__, "first") == 0);
	CHECK(logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
				     __FILE__, __LINE__, __func__, "second") == 0);
	logger_file_metrics_t m;
	CHECK(logger_get_file_metrics(log, &m) == 0);
	CHECK(m.rotation_attempts >= 1 && m.rotation_successes >= 1);
	CHECK(m.rotation_failures == 0);
	CHECK(logger_destroy_status(log) == 0);
	return 0;
}

static int audit_check(const char *dir, const char *name)
{
	char active[4096], state[4096], lock[4096];
	CHECK(snprintf(active, sizeof(active), "%s/%s.audit.log", dir, name) > 0);
	CHECK(snprintf(state, sizeof(state), "%s/%s.audit.state", dir, name) > 0);
	CHECK(snprintf(lock, sizeof(lock), "%s/%s.audit.lock", dir, name) > 0);
	(void)unlink(active);
	(void)unlink(state);
	(void)unlink(lock);

	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = dir;
	c.name = name;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(audit_init(&c) == 0);

	audit_event_t e = { 0 };
	e.phase = AUDIT_PHASE_RESULT;
	e.event = "MIN_KERNEL";
	e.actor = "ci";
	e.source = "qemu";
	e.resource = name;
	e.operation = "validate";
	e.result = AUDIT_SUCCESS;
	CHECK(audit_write(&e) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_verify_file(active) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 5)
		return 2;

	unsigned char random[32];
	CHECK(getrandom(random, sizeof(random), 0) == (ssize_t)sizeof(random));

	CHECK(rotation_check(argv[1]) == 0);
	CHECK(rotation_check(argv[2]) == 0);
	CHECK(audit_check(argv[3], "min-kernel-ext4") == 0);
	CHECK(audit_check(argv[4], "min-kernel-xfs") == 0);

	puts("MIN_KERNEL_RUNTIME_PROBE_OK");
	return 0;
}
