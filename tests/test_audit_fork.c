#include "audit.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void)
{
	unlink("./forkaudit.audit.log");
	unlink("./forkaudit.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "forkaudit";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 1;
	pid_t p = fork();
	if (p < 0)
		return 2;
	if (p == 0) {
		audit_after_fork_child();
		audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
				    .event = "CHILD",
				    .operation = "probe" };
		if (audit_write(&e) == 0)
			_exit(10);
		if (errno != ECHILD)
			_exit(11);
		_exit(0);
	}
	int st = 0;
	if (waitpid(p, &st, 0) < 0)
		return 3;
	audit_shutdown();
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 4;
}
