#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "./parent.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	if (logger_init(&cfg) != 0) {
		perror("logger_init");
		return 1;
	}
	LOG_INFO("initialized before fork");

	/* Application threads must already be stopped/joined here. Owners must
     * flush/destroy explicit loggers and audit_shutdown_status() first, if any.
     * The helper handles only the default logger (including its async worker).
     * Unlike raw fork after initialization, neither return branch needs exec.
     */
	pid_t pid = logger_fork_reinit();
	if (pid < 0) {
		int saved = errno;
		/* Teardown might already have happened. No automatic restart is done.
         * This example exits; a continuing application must decide how to
         * restore logging and handle any I/O failure, rather than ignore it. */
		logger_shutdown();
		errno = saved;
		perror("logger_fork_reinit");
		return 1;
	}

	cfg.file_path = pid == 0 ? "./child.log" : "./parent.log";
	int result = 0;
	if (logger_init(&cfg) != 0) {
		perror("logger_init after fork");
		result = 1;
	} else {
		LOG_INFO("%s reinitialized pid=%ld",
			 pid == 0 ? "child" : "parent", (long)getpid());
		if (logger_flush_status() != 0) {
			perror("logger_flush_status");
			result = 1;
		}
		logger_shutdown();
	}
	if (pid == 0)
		_exit(result);
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			perror("waitpid");
			return 1;
		}
	}
	return result || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}
