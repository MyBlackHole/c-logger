#include "logger.h"
#include "audit.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void)
{
	unlink("./guard.log");
	unlink("./guardaudit.audit.log");
	unlink("./guardaudit.audit.state");
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.file_path = "./guard.log";
	l.rotation.mode = LOGGER_ROTATE_NONE;
	if (logger_init(&l))
		return 1;
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	a.log_dir = ".";
	a.name = "guardaudit";
	a.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&a))
		return 2;
	/* Intentionally omit logger_prepare_fork(): this tests the safety net. */
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (p == 0) {
		LOG_INFO("must safely disappear");
		logger_flush();
		logger_shutdown();
		audit_event_t e = { .phase = AUDIT_PHASE_RESULT,
				    .event = "CHILD",
				    .operation = "probe" };
		if (audit_write(&e) == 0)
			_exit(10);
		if (errno != ECHILD)
			_exit(11);
		audit_shutdown();
		_exit(0);
	}
	int st = 0;
	if (waitpid(p, &st, 0) < 0)
		return 4;
	audit_shutdown();
	logger_shutdown();
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 5;
}
