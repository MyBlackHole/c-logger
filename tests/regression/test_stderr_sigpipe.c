#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include "support.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void configure_sigpipe(int blocked)
{
	struct sigaction action = { 0 };
	action.sa_handler = SIG_DFL;
	CHECK(sigemptyset(&action.sa_mask) == 0);
	CHECK(sigaction(SIGPIPE, &action, NULL) == 0);
	sigset_t set;
	CHECK(sigemptyset(&set) == 0);
	CHECK(sigaddset(&set, SIGPIPE) == 0);
	CHECK(pthread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) == 0);
}

static void check_sigpipe_state(int blocked, int was_pending)
{
	struct sigaction action;
	CHECK(sigaction(SIGPIPE, NULL, &action) == 0);
	CHECK(action.sa_handler == SIG_DFL);
	sigset_t current, pending;
	CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
	CHECK(sigismember(&current, SIGPIPE) == blocked);
	CHECK(sigpending(&pending) == 0);
	CHECK(sigismember(&pending, SIGPIPE) == was_pending);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	int async_mode = !strcmp(argv[1], "async");
	int bootstrap = !strncmp(argv[1], "bootstrap", 9);
	int healthy = !strcmp(argv[1], "bootstrap-format");
	int pending = !strcmp(argv[1], "bootstrap-pending");
	int preblocked = !strcmp(argv[1], "preblocked") || pending ||
		!strcmp(argv[1], "bootstrap-preblocked");
	CHECK(async_mode || preblocked || !strcmp(argv[1], "sync") ||
	      !strcmp(argv[1], "bootstrap") || healthy);

	int pipefd[2];
	CHECK(pipe(pipefd) == 0);
	if (!healthy)
		CHECK(close(pipefd[0]) == 0);
	CHECK(dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
	if (pipefd[1] != STDERR_FILENO)
		CHECK(close(pipefd[1]) == 0);

	configure_sigpipe(preblocked);
	if (pending)
		CHECK(raise(SIGPIPE) == 0);
	if (bootstrap) {
		errno = EDOM;
		logger_global_write(LOGGER_INFO, "sigpipe", __FILE__, __LINE__,
				    __func__, "bootstrap %s %d", "message", 42);
		CHECK(errno == (healthy ? EDOM : EPIPE));
		check_sigpipe_state(preblocked, pending);
		if (healthy) {
			char text[128];
			const char expected[] = "INFO  [sigpipe] bootstrap message 42\n";
			ssize_t n = read(pipefd[0], text, sizeof(text));
			CHECK(n == (ssize_t)(sizeof(expected) - 1));
			CHECK(!memcmp(text, expected, sizeof(expected) - 1));
			CHECK(close(pipefd[0]) == 0);
		}
		CHECK(logger_shutdown_status() == 0);
		return 0;
	}
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_STDERR;
	c.async_mode = async_mode;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	CHECK(l);

	if (async_mode) {
		LOGGER_INFO(l, "sigpipe", "broken stderr");
		CHECK(logger_flush_instance_status(l) == -1 && errno == EPIPE);
	} else {
		CHECK(logger_log_sync_status(l, LOGGER_INFO, "sigpipe", __FILE__,
					     __LINE__, __func__, "broken stderr") ==
		      -EPIPE);
	}
	check_sigpipe_state(preblocked, 0);

	logger_io_metrics_t io;
	logger_get_io_metrics(l, &io);
	CHECK(io.emitted_records == 0);
	CHECK(io.failed_records == 1);
	CHECK(io.first_error == EPIPE);

	errno = 0;
	CHECK(logger_destroy_status(l) == -1 && errno == EPIPE);
	check_sigpipe_state(preblocked, 0);
	return 0;
}
