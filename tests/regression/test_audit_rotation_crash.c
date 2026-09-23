#define _GNU_SOURCE
#include "record_support.h"

static int crash_armed;
void __real_logger_fault_crash_if_requested(const char *);
void __wrap_logger_fault_crash_if_requested(const char *point)
{
	if (crash_armed && !strcmp(point, "after_audit_fsync"))
		_exit(197);
	__real_logger_fault_crash_if_requested(point);
}

static int child(const char *mode, const char *algorithm)
{
	audit_config_t c = audit_test_config();
	c.integrity = crypto_algorithm(algorithm);
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 750;
	CHECK(audit_init(&c) == 0);
	if (!strcmp(mode, "--writer")) {
		for (unsigned i = 0; i < 8; ++i) {
			audit_event_t e = audit_test_event("BEFORE_CRASH");
			CHECK(audit_write(&e) == 0);
		}
		crash_armed = 1;
		audit_event_t e = audit_test_event("CRASH_TARGET");
		(void)audit_write(&e);
		CHECK(0);
	}
	CHECK(!strcmp(mode, "--recover"));
	CHECK(audit_shutdown_status() == 0);
	return 0;
}

static void run_child(const char *mode, const char *alg, int expected)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
	CHECK(n > 0 && (size_t)n < sizeof(exe) - 1u);
	exe[n] = 0;
	char *const args[] = { exe, (char *)mode, (char *)alg, NULL };
	pid_t p = fork();
	CHECK(p >= 0);
	if (!p) {
		execv(exe, args);
		_exit(127);
	}
	int status;
	CHECK(waitpid(p, &status, 0) == p);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	if (!strncmp(argv[1], "--", 2))
		return child(argv[1], argv[2]);
	CHECK(!strcmp(argv[1], "rotation"));
	char dir[] = "/tmp/audit-rotation-crash-XXXXXX";
	enter_temp(dir);
	run_child("--writer", argv[2], 197);
	run_child("--recover", argv[2], 0);
	run_child("--recover", argv[2], 0);
	DIR *d = opendir(".");
	CHECK(d);
	unsigned targets = 0, archives = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strstr(e->d_name, ".log")) {
			FILE *f = fopen(e->d_name, "r");
			CHECK(f);
			char line[AUDIT_RECORD_MAX + 1u];
			while (fgets(line, sizeof(line), f))
				if (strstr(line, "event=\"CRASH_TARGET\""))
					++targets;
			CHECK(!ferror(f) && fclose(f) == 0);
			if (strcmp(e->d_name, "app.audit.log"))
				++archives;
		}
	CHECK(closedir(d) == 0 && targets == 1 && archives >= 4);
	audit_test_cleanup(dir);
	puts("real rotation+post-fsync process crash recovery passed");
	return 0;
}
