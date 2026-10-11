#include "logger.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void)
{
	unlink("./guard.log");
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.file_path = "./guard.log";
	l.rotation.mode = LOGGER_ROTATE_NONE;
	if (logger_init(&l))
		return 1;
	/* Fork with a live runtime: the internal atfork/PID guard is the safety net. */
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (p == 0) {
		LOG_INFO("must safely disappear");
		if (logger_flush_status() != -1 || errno != ECHILD)
			_exit(10);
		if (logger_shutdown_status() != -1 || errno != ECHILD)
			_exit(11);
		_exit(0);
	}
	int st = 0;
	if (waitpid(p, &st, 0) < 0)
		return 4;
	logger_shutdown();
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 5;
}
