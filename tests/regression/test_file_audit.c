#define _GNU_SOURCE
#include "audit_support.h"
#include "audit_internal.h"
#include "logger_internal.h"
#include <signal.h>

static char exe[4096];
static audit_config_t cfg(const char *name, audit_integrity_t alg)
{
	audit_config_t c = audit_test_config();
	c.name = name;
	c.integrity = alg;
	return c;
}
static int subprocess(char **args, int expected)
{
	pid_t p = fork();
	CHECK(p >= 0);
	if (!p) {
		execv(exe, args);
		_exit(127);
	}
	int st;
	CHECK(waitpid(p, &st, 0) == p && WIFEXITED(st) &&
	      WEXITSTATUS(st) == expected);
	return 0;
}
static int child(int argc, char **argv)
{
	CHECK(argc == 4);
	CHECK(!strcmp(argv[3], "sha256"));
	audit_integrity_t alg = AUDIT_INTEGRITY_SHA256;
	audit_config_t c = cfg("app", alg);
	if (!strcmp(argv[1], "--crash")) {
		c.rotation.mode = LOGGER_ROTATE_SIZE;
		c.rotation.max_file_size = 1;
		c.rotation.retention_days = 0;
		CHECK(setenv("LOGGER_CRASH_POINT", argv[2], 1) == 0);
		CHECK(audit_init(&c) == 0);
		CHECK(!"requested crash hook was not reached");
	}
	if (!strcmp(argv[1], "--state-busy")) {
		c.name = "other";
		c.chain_state_path = "app.audit.state";
		CHECK(audit_init(&c) == -1 && errno == EBUSY);
		return 0;
	}
	CHECK(0);
	return 1;
}
static void crash(const char *point, audit_integrity_t alg)
{
	audit_config_t c = cfg("app", alg);
	CHECK(audit_init(&c) == 0);
	audit_event_t e = audit_test_event("BASELINE");
	CHECK(audit_write(&e) == 0 && audit_shutdown_status() == 0);
	audit_ckpt_t before;
	CHECK(audit_checkpoint_load("app.audit.state", &before) == 0);
	char *args[] = { exe, "--crash", (char *)point, "sha256", NULL };
	subprocess(args, 197);
	CHECK(audit_init(&c) == 0);
	e.event = "RECOVERED";
	CHECK(audit_write(&e) == 0);
	CHECK(audit_shutdown_status() == 0);
	audit_ckpt_t after;
	CHECK(audit_checkpoint_load("app.audit.state", &after) == 0);
	CHECK(memcmp(before.hash, after.hash, 32) != 0);
	CHECK(audit_recover_set(".", "app", "app.audit.log", &after) == 0);
	CHECK(count_event("RECOVERED") == 1);
}
static void logger_blocks_audit(int state)
{
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.async_mode = 0;
	l.rotation.mode = LOGGER_ROTATE_NONE;
	l.file_path = state ? "app.audit.state" : "app.audit.log";
	logger_t *owner = logger_create(&l);
	CHECK(owner);
	LOGGER_INFO(owner, "owner", "not-a-valid-audit-record");
	CHECK(logger_flush_instance_status(owner) == 0);
	off_t size = file_size(l.file_path);
	audit_config_t c = cfg("app", AUDIT_INTEGRITY_SHA256);
	CHECK(audit_init(&c) == -1 && errno == EBUSY);
	CHECK(file_size(l.file_path) == size);
	if (!state)
		CHECK(access("app.audit.state", F_OK) != 0);
	CHECK(logger_destroy_status(owner) == 0);
}
static void audit_blocks_logger(void)
{
	audit_config_t c = cfg("app", AUDIT_INTEGRITY_SHA256);
	CHECK(audit_init(&c) == 0);
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.async_mode = 0;
	l.rotation.mode = LOGGER_ROTATE_NONE;
	l.file_path = "app.audit.log";
	CHECK(logger_create(&l) == NULL && errno == EBUSY);
	char *args[] = { exe, "--state-busy", "unused", "sha256", NULL };
	subprocess(args, 0);
	audit_event_t e = audit_test_event("OWNER_OK");
	CHECK(audit_write(&e) == 0 && audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}
static void audit_cwd(void)
{
	CHECK(mkdir("a", 0700) == 0 && mkdir("b", 0700) == 0);
	audit_config_t c = cfg("app", AUDIT_INTEGRITY_SHA256);
	c.log_dir = "a";
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 500;
	c.rotation.retention_days = 0;
	CHECK(audit_init(&c) == 0);
	CHECK(rename("a", "moved") == 0 && chdir("b") == 0);
	audit_event_t e = audit_test_event("BOUND_DIRECTORY");
	CHECK(audit_write(&e) == 0 && audit_shutdown_status() == 0);
	CHECK(access("app.audit.state", F_OK) != 0 &&
	      access("app.audit.log", F_OK) != 0);
	CHECK(chdir("..") == 0);
	audit_ckpt_t cp;
	CHECK(audit_checkpoint_load("moved/app.audit.state", &cp) == 0);
	CHECK(audit_recover_set("moved", "app", "moved/app.audit.log", &cp) ==
	      0);
	/* remove nested fixtures using the existing Audit test helper */
	CHECK(chdir("moved") == 0);
	DIR *d = opendir(".");
	CHECK(d);
	struct dirent *entry;
	while ((entry = readdir(d)))
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			CHECK(unlink(entry->d_name) == 0);
	CHECK(closedir(d) == 0 && chdir("..") == 0 && rmdir("moved") == 0 &&
	      rmdir("b") == 0);
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strncmp(argv[1], "--", 2))
		return child(argc, argv);
	CHECK(argc == 2 || argc == 3);
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	CHECK(n > 0);
	exe[n] = 0;
	char dir[] = "/tmp/file-audit-XXXXXX";
	enter_temp(dir);
	if (!strcmp(argv[1], "logger-busy"))
		logger_blocks_audit(0);
	else if (!strcmp(argv[1], "state-busy"))
		logger_blocks_audit(1);
	else if (!strcmp(argv[1], "audit-busy"))
		audit_blocks_logger();
	else if (!strcmp(argv[1], "cwd"))
		audit_cwd();
	else {
		CHECK(argc == 3 && !strcmp(argv[2], "sha256"));
		crash(argv[1], AUDIT_INTEGRITY_SHA256);
	}
	audit_test_cleanup(dir);
	return 0;
}
