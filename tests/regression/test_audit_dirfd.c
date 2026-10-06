#define _GNU_SOURCE
#include "audit_support.h"

#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>

int __real_stat(const char *, struct stat *);

int __wrap_stat(const char *path, struct stat *st)
{
	if (path && !strncmp(path, "/proc/self/fd", 13)) {
		errno = EACCES;
		return -1;
	}
	return __real_stat(path, st);
}

int main(void)
{
	char root[] = "/tmp/audit-dirfd-XXXXXX";
	enter_temp(root);
	CHECK(mkdir("log", 0700) == 0);
	CHECK(mkdir("state", 0700) == 0);

	audit_config_t c = audit_test_config();
	c.log_dir = "log";
	c.name = "app";
	c.chain_state_path = "state/app.audit.state";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	CHECK(audit_init(&c) == 0);

	/* Runtime identity must follow owned directory descriptors, not the
	 * original parent path or the caller's current working directory. */
	CHECK(rename("log", "log-moved") == 0);
	CHECK(rename("state", "state-moved") == 0);
	CHECK(chdir("/") == 0);

	audit_event_t e = { 0 };
	e.phase = AUDIT_PHASE_RESULT;
	e.event = "DIRFD";
	e.actor = "test";
	e.source = "regression";
	e.resource = "app";
	e.operation = "write-after-parent-rename";
	e.result = AUDIT_SUCCESS;
	CHECK(audit_write(&e) == 0);
	CHECK(audit_flush() == 0);
	CHECK(audit_shutdown_status() == 0);

	char log_path[PATH_MAX], state_path[PATH_MAX], path[PATH_MAX];
	CHECK(snprintf(log_path, sizeof(log_path), "%s/log-moved/app.audit.log",
		       root) > 0);
	CHECK(snprintf(state_path, sizeof(state_path),
		       "%s/state-moved/app.audit.state", root) > 0);
	CHECK(audit_verify_file(log_path) == 0);
	struct stat st;
	CHECK(stat(state_path, &st) == 0 && S_ISREG(st.st_mode));

	const char *files[] = {
		"log-moved/app.audit.log",
		"log-moved/app.audit.log.logger.lock",
		"state-moved/app.audit.state",
		"state-moved/app.audit.state.logger.lock",
	};
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
		CHECK(snprintf(path, sizeof(path), "%s/%s", root, files[i]) > 0);
		CHECK(unlink(path) == 0);
	}
	CHECK(snprintf(path, sizeof(path), "%s/log-moved", root) > 0);
	CHECK(rmdir(path) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/state-moved", root) > 0);
	CHECK(rmdir(path) == 0);
	CHECK(rmdir(root) == 0);
	return 0;
}
