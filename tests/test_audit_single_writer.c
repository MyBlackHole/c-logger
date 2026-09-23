#define _GNU_SOURCE
#include "audit.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "single";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (argc == 2 && !strcmp(argv[1], "--contender")) {
		if (audit_init(&c) != -1 || errno != EBUSY)
			return 10;
		return 0;
	}
	unlink("single.audit.log");
	unlink("single.audit.state");
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n < 0 || (size_t)n >= sizeof(exe) - 1)
		return 1;
	exe[n] = 0;
	if (audit_init(&c))
		return 2;
	char *args[] = { exe, "--contender", NULL };
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (!p) {
		execv(exe, args);
		_exit(127);
	}
	int st;
	if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st))
		return 4;
	if (audit_shutdown_status())
		return 5;
	if (audit_init(&c))
		return 6;
	return audit_shutdown_status() ? 7 : 0;
}
