#define _GNU_SOURCE
#include "console.h"
#include "support.h"
#include <locale.h>
#include <signal.h>
#include <sys/wait.h>
#include <wchar.h>

/* No wrappers or private entry points: run against the shipped static/DSO
 * library. Each child starts before the parent has entered the runtime. */
static int report_fd = STDERR_FILENO;
#undef CHECK
#define CHECK(expr)                                                       \
	do {                                                              \
		if (!(expr)) {                                             \
			dprintf(report_fd, "%s:%d: %s (errno=%d)\n",       \
				__FILE__, __LINE__, #expr, errno);          \
			_exit(1);                                          \
		}                                                         \
	} while (0)

static volatile sig_atomic_t signal_calls;
static void sigpipe_handler(int signum)
{
	(void)signum;
	++signal_calls;
}

static const char *const names[] = {
	"print", "info", "warn", "error", "verbose", "debug", "source"
};
static const char *const labels[] = {
	"", "INFO: ", "WARN: ", "ERROR: ", "INFO: ", "DEBUG: ",
	"DEBUG: console_source.c:42 caller() "
};

typedef int (*output_fn)(const char *, ...);
static const output_fn outputs[] = {
	console_print, console_info, console_warn, console_error,
	console_verbose, console_debug
};

static int output(unsigned api)
{
	/* The function-pointer type intentionally has no printf attribute: %m
	 * is the supported libc extension under test, not an ISO format. */
	if (api < sizeof(outputs) / sizeof(outputs[0]))
		return outputs[api]("message %s %d errno=%m", "text", 42);
	const char *fmt = "message %s %d errno=%m";
	return console_debug_source("/tmp/console_source.c", 42, "caller",
				    fmt, "text", 42);
}

static void configure(const char *scenario, sigset_t *expected_mask,
		      struct sigaction *expected_action)
{
	struct sigaction action = { 0 };
	action.sa_handler = !strcmp(scenario, "handler") ? sigpipe_handler :
		!strcmp(scenario, "ignored") ? SIG_IGN : SIG_DFL;
	action.sa_flags = SA_RESTART;
	CHECK(!sigemptyset(&action.sa_mask));
	CHECK(!sigaddset(&action.sa_mask, SIGUSR2));
	CHECK(!sigaction(SIGPIPE, &action, NULL));
	CHECK(!sigaction(SIGPIPE, NULL, expected_action));
	sigset_t mask;
	CHECK(!sigemptyset(&mask));
	CHECK(!sigaddset(&mask, SIGUSR1));
	if (!strcmp(scenario, "preblocked") || !strcmp(scenario, "pending"))
		CHECK(!sigaddset(&mask, SIGPIPE));
	CHECK(!pthread_sigmask(SIG_SETMASK, &mask, NULL));
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, expected_mask));
	if (!strcmp(scenario, "pending"))
		CHECK(!raise(SIGPIPE));
}

static void check_set(const sigset_t *actual, const sigset_t *expected)
{
	for (int signum = 1; signum < NSIG; ++signum)
		CHECK(sigismember(actual, signum) ==
		      sigismember(expected, signum));
}

static void check_signal_state(const sigset_t *expected_mask,
			       const struct sigaction *expected_action,
			       int was_pending)
{
	sigset_t mask;
	sigset_t pending;
	struct sigaction action;
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
	check_set(&mask, expected_mask);
	CHECK(!sigpending(&pending));
	CHECK(sigismember(&pending, SIGPIPE) == was_pending);
	CHECK(!sigaction(SIGPIPE, NULL, &action));
	CHECK(action.sa_handler == expected_action->sa_handler);
	CHECK(action.sa_flags == expected_action->sa_flags);
	check_set(&action.sa_mask, &expected_action->sa_mask);
	CHECK(signal_calls == 0);
}

static void run_case(const char *scenario, unsigned api, int buffered)
{
	report_fd = dup(STDERR_FILENO);
	CHECK(report_fd >= 0);
	int pipefd[2];
	CHECK(!pipe(pipefd));
	int healthy = !strcmp(scenario, "healthy") ||
		!strcmp(scenario, "host-buffer");
	int first_error = !strcmp(scenario, "first-error");
	int host_flush = !strcmp(scenario, "host-flush");
	if (!healthy)
		CHECK(!close(pipefd[0]));
	int fd = api == 0 ? STDOUT_FILENO : STDERR_FILENO;
	FILE *stream = api == 0 ? stdout : stderr;
	CHECK(dup2(pipefd[1], fd) == fd);
	CHECK(!close(pipefd[1]));
	char buffer[512];
	CHECK(!setvbuf(stream, buffer, buffered ? _IOFBF : _IONBF,
		      sizeof(buffer)));

	sigset_t expected_mask;
	struct sigaction expected_action;
	configure(scenario, &expected_mask, &expected_action);
	console_config_t config = CONSOLE_DEFAULT_CONFIG();
	config.color = CONSOLE_COLOR_AUTO;
	config.verbosity = CONSOLE_DEBUG;
	console_init(&config);
	if (!strcmp(scenario, "host-buffer") || host_flush || first_error)
		CHECK(fputs("host-prefix|", stream) >= 0);
	errno = EDOM;
	int rc;
	if (first_error) {
		CHECK(api < sizeof(outputs) / sizeof(outputs[0]));
		CHECK(setlocale(LC_CTYPE, "C") != NULL);
		rc = outputs[api]("prefix %lc", (wint_t)0x100);
	} else if (host_flush) {
		/* Even an empty Console write must protect its fflush of bytes
		 * that the host had already placed in the shared FILE buffer. */
		rc = console_print("%s", "");
	} else {
		rc = output(api);
	}
	CHECK(rc == (healthy ? 0 : -1));
	CHECK(errno == (healthy ? EDOM : first_error ? EILSEQ : EPIPE));
	CHECK(!!ferror(stream) == !healthy);
	check_signal_state(&expected_mask, &expected_action,
			   !strcmp(scenario, "pending"));
	if (healthy) {
		CHECK(!close(fd));
		char actual[1024];
		ssize_t size = read(pipefd[0], actual, sizeof(actual) - 1);
		CHECK(size >= 0);
		actual[size] = '\0';
		char expected[1024];
		int length = snprintf(expected, sizeof(expected),
				      "%s%smessage text 42 errno=%s%s",
			!strcmp(scenario, "host-buffer") ? "host-prefix|" : "",
			labels[api], strerror(EDOM), api == 0 ? "" : "\n");
		CHECK(length >= 0 && (size_t)length < sizeof(expected));
		CHECK(size == length && !strcmp(actual, expected));
		CHECK(read(pipefd[0], actual, sizeof(actual)) == 0);
		CHECK(!close(pipefd[0]));
	}
	_exit(0);
}

static _Atomic int callback_entered;
static _Atomic int callback_release;
static _Atomic int cleanup_seen;
static int broken_fd;
static ssize_t blocked_write(void *cookie, const char *text, size_t size)
{
	(void)cookie;
	sigset_t mask;
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
	CHECK(sigismember(&mask, SIGPIPE) == 1);
	atomic_store(&callback_entered, 1);
	wait_flag(&callback_release);
	return write(broken_fd, text, size);
}

static void check_thread_restored(void *expected)
{
	sigset_t mask;
	sigset_t pending;
	CHECK(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
	check_set(&mask, expected);
	CHECK(!sigpending(&pending));
	CHECK(sigismember(&pending, SIGPIPE) == 0);
	atomic_store(&cleanup_seen, 1);
}

static void *print_thread(void *expected)
{
	pthread_cleanup_push(check_thread_restored, expected);
	errno = EDOM;
	CHECK(console_print("callback") == -1 && errno == EPIPE);
	pthread_testcancel();
	pthread_cleanup_pop(1);
	return NULL;
}

static void check_thread_local(int cancel)
{
	sigset_t expected_mask;
	struct sigaction expected_action;
	configure("handler", &expected_mask, &expected_action);
	int pipefd[2];
	CHECK(!pipe(pipefd));
	CHECK(!close(pipefd[0]));
	broken_fd = pipefd[1];
	cookie_io_functions_t ops = { .write = blocked_write };
	FILE *stream = fopencookie(NULL, "w", ops);
	CHECK(stream);
	CHECK(!setvbuf(stream, NULL, _IONBF, 0));
	FILE *old_stdout = stdout;
	stdout = stream;
	pthread_t writer;
	CHECK(!pthread_create(&writer, NULL, print_thread, &expected_mask));
	wait_flag(&callback_entered);
	check_signal_state(&expected_mask, &expected_action, 0);
	CHECK(!raise(SIGPIPE));
	CHECK(signal_calls == 1);
	if (cancel)
		CHECK(!pthread_cancel(writer));
	atomic_store(&callback_release, 1);
	void *result;
	CHECK(!pthread_join(writer, &result));
	CHECK(result == (cancel ? PTHREAD_CANCELED : NULL));
	CHECK(atomic_load(&cleanup_seen));
	CHECK(signal_calls == 1);
	stdout = old_stdout;
	CHECK(!fclose(stream));
	CHECK(!close(broken_fd));
}

static void check_large_output(void)
{
	FILE *stream = tmpfile();
	CHECK(stream);
	FILE *old_stdout = stdout;
	stdout = stream;
	char text[32768];
	memset(text, 'x', sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	errno = EDOM;
	CHECK(!console_print("%s", text) && errno == EDOM);
	CHECK(ftell(stream) == (long)(sizeof(text) - 1));
	CHECK(!fseek(stream, 0, SEEK_SET));
	for (size_t i = 0; i < sizeof(text) - 1; ++i)
		CHECK(fgetc(stream) == 'x');
	CHECK(fgetc(stream) == EOF && !ferror(stream));
	stdout = old_stdout;
	CHECK(!fclose(stream));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *scenario = argv[1];
	if (!strcmp(scenario, "thread-local") || !strcmp(scenario, "cancel")) {
		check_thread_local(!strcmp(scenario, "cancel"));
		return 0;
	}
	if (!strcmp(scenario, "large-output")) {
		check_large_output();
		return 0;
	}
	CHECK(!strcmp(scenario, "closed") ||
	      !strcmp(scenario, "preblocked") || !strcmp(scenario, "pending") ||
	      !strcmp(scenario, "handler") || !strcmp(scenario, "ignored") ||
	      !strcmp(scenario, "healthy") || !strcmp(scenario, "first-error") ||
	      !strcmp(scenario, "host-buffer") || !strcmp(scenario, "host-flush"));
	for (unsigned api = 0; api < sizeof(names) / sizeof(names[0]); ++api) {
		if (!strcmp(scenario, "first-error") && api == 6)
			continue;
		if (!strcmp(scenario, "host-flush") && api != 0)
			continue;
		for (int buffered = 0; buffered <= 1; ++buffered) {
			if (!buffered && (!strcmp(scenario, "first-error") ||
					  !strcmp(scenario, "host-buffer") ||
					  !strcmp(scenario, "host-flush")))
				continue;
			pid_t pid = fork();
			CHECK(pid >= 0);
			if (!pid)
				run_case(scenario, api, buffered);
			int status;
			CHECK(waitpid(pid, &status, 0) == pid);
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				fprintf(stderr, "%s/%s/buffered=%d: status=%d\n",
					scenario, names[api], buffered, status);
				return 1;
			}
		}
	}
	return 0;
}
