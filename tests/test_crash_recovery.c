#define _GNU_SOURCE
#include "audit.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int do_write(void)
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "crash";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 1;
	audit_event_t e = { .event = "DELETE",
			    .actor = "admin",
			    .source = "local",
			    .resource = "key-1",
			    .operation = "delete" };
	if (audit_begin(&e) || audit_end(&e, AUDIT_SUCCESS, 0))
		return 2;
	return audit_shutdown_status();
}
int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--crash-worker")) {
		/* Fresh exec image; set environment only while single-threaded. */
		if (setenv("LOGGER_CRASH_POINT", argv[2], 1))
			return 98;
		return do_write() ?
			       99 :
			       0; /* crash hook should terminate with 197 */
	}
	if (argc != 2)
		return 97;
	unlink("crash.audit.log");
	unlink("crash.audit.state");
	unsetenv("LOGGER_CRASH_POINT");
	if (do_write())
		return 1;
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n < 0 || (size_t)n >= sizeof(exe) - 1)
		return 2;
	exe[n] = 0;
	char *args[] = { exe, "--crash-worker", argv[1], NULL };
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (!p) {
		execv(exe, args);
		_exit(127);
	}
	int status;
	if (waitpid(p, &status, 0) != p || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 197)
		return 4;
	if (do_write()) {
		perror("restart");
		return 5;
	}
	if (audit_verify_file("crash.audit.log")) {
		perror("verify");
		return 6;
	}
	return 0;
}
