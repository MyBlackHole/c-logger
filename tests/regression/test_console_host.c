#define _GNU_SOURCE
#include "logger.h"
#include "console.h"
#include "support.h"
#include <stdint.h>
/* These tests use a real application-supplied FILE callback. No --wrap;
 * shared builds link and execute the production DSO. */
static _Atomic int entered, release_io, cleanup_seen, arm, config_run;
static _Atomic unsigned writes, escapes;
static int reentry;
static logger_t *log_instance;
static ssize_t host_write(void *user, const char *text, size_t size)
{
	(void)user;
	atomic_fetch_add(&writes, 1);
	for (size_t i = 0; i < size; ++i)
		if (text[i] == '\033')
			atomic_fetch_add(&escapes, 1);
	if (atomic_exchange(&arm, 0)) {
		if (reentry) {
			errno = 0;
			CHECK(console_print("recursive") == -1 &&
			      errno == EDEADLK);
			errno = 0;
			CHECK(logger_flush_instance_status(log_instance) ==
				      -1 &&
			      errno == EDEADLK);
			errno = 0;
			CHECK(logger_shutdown_status() == -1 &&
			      errno == EDEADLK);
		} else {
			atomic_store(&entered, 1);
			wait_flag(&release_io);
		}
	}
	return (ssize_t)size;
}
static void cleanup(void *unused)
{
	(void)unused;
	int old;
	CHECK(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old));
	CHECK(!console_print("cleanup\n"));
	LOGGER_INFO(log_instance, "cleanup", "after-cancel");
	CHECK(!logger_flush_instance_status(log_instance));
	atomic_store(&cleanup_seen, 1);
}
static void *print_thread(void *unused)
{
	(void)unused;
	pthread_cleanup_push(cleanup, NULL);
	CHECK(!console_print("blocked\n"));
	pthread_testcancel();
	pthread_cleanup_pop(0);
	return NULL;
}
static void *configure(void *id)
{
	console_config_t cfg = CONSOLE_DEFAULT_CONFIG();
	if ((uintptr_t)id == 1) {
		cfg.verbosity = CONSOLE_QUIET;
		cfg.color = CONSOLE_COLOR_ALWAYS;
	} else {
		cfg.verbosity = CONSOLE_DEBUG;
		cfg.color = CONSOLE_COLOR_NEVER;
	}
	while (atomic_load(&config_run))
		console_init(&cfg);
	return NULL;
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	unlink("host.log");
	unlink("host.log.logger.lock");
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "host.log";
	cfg.async_mode = 1;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	log_instance = logger_create(&cfg);
	CHECK(log_instance);
	FILE *old_stdout = stdout, *old_stderr = stderr;
	cookie_io_functions_t ops = { .write = host_write };
	FILE *stream = fopencookie(NULL, "w", ops);
	CHECK(stream);
	CHECK(!setvbuf(stream, NULL, _IONBF, 0));
	stdout = stream;
	stderr = stream;
	console_config_t cc = CONSOLE_DEFAULT_CONFIG();
	cc.verbosity = CONSOLE_DEBUG;
	cc.color = CONSOLE_COLOR_NEVER;
	console_init(&cc);
	if (!strcmp(argv[1], "cancel")) {
		atomic_store(&arm, 1);
		pthread_t t;
		CHECK(!pthread_create(&t, NULL, print_thread, NULL));
		wait_flag(&entered);
		CHECK(!pthread_cancel(t));
		for (int i = 0; i < 10; ++i)
			nap_ms();
		CHECK(!atomic_load(&cleanup_seen));
		atomic_store(&release_io, 1);
		void *value;
		CHECK(!pthread_join(t, &value));
		CHECK(value == PTHREAD_CANCELED);
		CHECK(atomic_load(&cleanup_seen));
	} else if (!strcmp(argv[1], "reentry")) {
		reentry = 1;
		atomic_store(&arm, 1);
		CHECK(!console_print("outer\n"));
		CHECK(!atomic_load(&arm));
	} else {
		CHECK(!strcmp(argv[1], "config-snapshot"));
		atomic_store(&config_run, 1);
		pthread_t a, b;
		CHECK(!pthread_create(&a, NULL, configure,
				      (void *)(uintptr_t)1));
		CHECK(!pthread_create(&b, NULL, configure,
				      (void *)(uintptr_t)2));
		for (int i = 0; i < 20000; ++i)
			CHECK(!console_debug("snapshot=%d", i));
		atomic_store(&config_run, 0);
		CHECK(!pthread_join(a, NULL));
		CHECK(!pthread_join(b, NULL));
		console_init(&cc);
		CHECK(!console_debug("final"));
		CHECK(atomic_load(&writes) > 0);
		CHECK(atomic_load(&escapes) == 0);
	}
	CHECK(!console_print("still-usable\n"));
	stdout = old_stdout;
	stderr = old_stderr;
	CHECK(!fclose(stream));
	CHECK(!logger_destroy_status(log_instance));
	puts("real host stdio callback integration passed");
	return 0;
}
