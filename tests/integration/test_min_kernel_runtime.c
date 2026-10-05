#define _GNU_SOURCE
#include <audit.h>
#include <logger.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr)                                                        \
	do {                                                               \
		if (!(expr)) {                                             \
			fprintf(stderr, "CHECK failed: %s line=%d errno=%d\n", \
				#expr, __LINE__, errno);                       \
			return -1;                                         \
		}                                                          \
	} while (0)

static int procfd_check(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	char proc[64], target[4096];
	CHECK(snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd) > 0);
	ssize_t n = readlink(proc, target, sizeof(target) - 1u);
	CHECK(n > 0 && (size_t)n < sizeof(target));
	target[n] = 0;
	CHECK(close(fd) == 0);
	printf("procfd dir=%s target=%s\n", dir, target);
	return 0;
}

static int rotation_check(const char *path, int expect_supported)
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
				     __FILE__, __LINE__, __func__,
				     "first") == 0);
	int rc = logger_log_sync_status(log, LOGGER_INFO, "min-kernel",
					__FILE__, __LINE__, __func__,
					"second");
	logger_file_metrics_t m;
	CHECK(logger_get_file_metrics(log, &m) == 0);
	if (expect_supported) {
		CHECK(rc == 0);
		CHECK(m.rotation_attempts >= 1 && m.rotation_successes >= 1);
		CHECK(m.rotation_failures == 0);
	} else {
		CHECK(rc == -ENOTSUP);
		CHECK(m.rotation_attempts >= 1 && m.rotation_successes == 0);
		CHECK(m.rotation_failures >= 1);
	}
	(void)logger_destroy_status(log);
	printf("rotation path=%s supported=%d attempts=%llu successes=%llu failures=%llu\n",
	       path, expect_supported,
	       (unsigned long long)m.rotation_attempts,
	       (unsigned long long)m.rotation_successes,
	       (unsigned long long)m.rotation_failures);
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
	printf("audit dir=%s verified\n", dir);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s EXT4_LOG XFS_LOG EXT4_DIR XFS_DIR\n", argv[0]);
		return 2;
	}
	unsigned char random[32];
	CHECK(getrandom(random, sizeof(random), 0) == (ssize_t)sizeof(random));
	CHECK(procfd_check(argv[3]) == 0);
	CHECK(procfd_check(argv[4]) == 0);
	CHECK(rotation_check(argv[1], 1) == 0);
	/* Upstream XFS gained RENAME_NOREPLACE in Linux 4.0. Linux 3.17 is
	 * supported for XFS only when internal rotation is not requested; the
	 * backend must fail closed with ENOTSUP rather than overwrite an archive. */
	CHECK(rotation_check(argv[2], 0) == 0);
	CHECK(audit_check(argv[3], "min-kernel-ext4") == 0);
	CHECK(audit_check(argv[4], "min-kernel-xfs") == 0);
	puts("MIN_KERNEL_RUNTIME_PROBE_OK");
	return 0;
}
