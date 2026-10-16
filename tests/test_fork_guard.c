#include <errno.h>
#include "regression/default_fixture.h"
static logger_t *default_owner;
#include "logger.h"
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void)
{
	if (logger_process_init() != 0)
		return 1;
	unlink("./guard.log");
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.file_path = "./guard.log";
	l.rotation.mode = LOGGER_ROTATE_NONE;
	if (fixture_default_create(&l, &default_owner))
		return 1;
	/* 运行时存活时 fork：内部 atfork/PID 守卫提供安全保障。 */
	pid_t p = fork();
	if (p < 0)
		return 3;
	if (p == 0) {
		LOG_INFO("must safely disappear");
		if (logger_flush_status() != -1 || errno != ECHILD)
			_exit(10);
		if (logger_default_set(NULL, NULL) != -ECHILD)
			_exit(11);
		_exit(0);
	}
	int st = 0;
	if (waitpid(p, &st, 0) < 0)
		return 4;
	fixture_default_destroy(&default_owner);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 5;
}
