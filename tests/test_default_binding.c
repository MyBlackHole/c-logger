#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <dirent.h>
#include <sys/wait.h>

static logger_t *make_logger(const char *path, int async)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();

	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = path;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.async_mode = async;
	cfg.queue_capacity = 32;
	logger_t *owner = NULL;

	CHECK(logger_create(&cfg, &owner) == 0);
	return owner;
}

static unsigned int lines(const char *path)
{
	FILE *f = fopen(path, "rb");

	CHECK(f);
	unsigned int result = 0;
	int ch;

	while ((ch = fgetc(f)) != EOF)
		result += ch == '\n';
	CHECK(!ferror(f) && fclose(f) == 0);
	return result;
}

static void absent(void)
{
	int touched = 42;

	errno = EDOM;
	CHECK(LOG_INFO("must-not-format%n", &touched) == -ENODEV);
	CHECK(errno == EDOM && touched == 42);
	CHECK(logger_default_flush() == -ENODEV);
}

static void healthy(int async)
{
	absent();
	errno = EDOM;
	CHECK(logger_default_set(NULL, NULL) == 0 && errno == EDOM);
	logger_t *a = make_logger("a.log", async);
	logger_t *b = make_logger("b.log", async);

	errno = ERANGE;
	CHECK(logger_default_set(NULL, a) == 0 && errno == ERANGE);
	LOGGER_INFO(a, "explicit", "A_EXPLICIT");
	LOG_INFO("A_DEFAULT");
	logger_t *original = a;

	CHECK(logger_destroy(&a) == -EBUSY && a == original);
	CHECK(errno == ERANGE);
	CHECK(logger_default_set(NULL, b) == -ESTALE && errno == ERANGE);
	CHECK(logger_default_set(a, a) == 0);
	CHECK(logger_default_set(a, b) == 0);
	logger_state_t state;

	CHECK(logger_get_state(a, &state) == 0 &&
	      state == LOGGER_STATE_RUNNING);
	CHECK(logger_destroy(&a) == 0 && a == NULL);
	CHECK(lines("a.log") == 2);
	CHECK(file_contains("a.log", "A_DEFAULT"));
	LOG_INFO("B_DEFAULT");
	CHECK(logger_default_flush() == 0);
	CHECK(logger_default_set(b, NULL) == 0);
	absent();
	LOGGER_INFO(b, "explicit", "B_STILL_OWNED");
	CHECK(logger_get_state(b, &state) == 0 &&
	      state == LOGGER_STATE_RUNNING);
	CHECK(logger_destroy(&b) == 0 && !b);
	CHECK(lines("b.log") == 2);
}

static void io_failure(void)
{
	logger_t *l = make_logger("/dev/full", 0);

	CHECK(logger_default_set(NULL, l) == 0);
	errno = EDOM;
	CHECK(LOG_ERROR("expected-output-failure") < 0 && errno == EDOM);
	logger_io_metrics_t io;

	CHECK(logger_get_io_metrics(l, &io) == 0 && errno == EDOM);
	CHECK(io.first_error != 0);
	errno = EDOM;
	/* 解绑只归还借用，不擅自刷新，亦不代替宿主报告最终 I/O 错误。 */
	CHECK(logger_default_set(l, NULL) == 0 && errno == EDOM);
	CHECK(logger_destroy(&l) == -io.first_error && l == NULL);
	CHECK(errno == EDOM);
}

static void fork_case(void)
{
	/* 父方保持单线程；本用例没有检测器的多线程 fork 新 worker 豁免。 */
	logger_t *a = make_logger("a.log", 0);

	CHECK(logger_default_set(NULL, a) == 0);
	LOG_INFO("PARENT_BEFORE");
	pid_t child = fork();

	CHECK(child >= 0);
	if (!child) {
		if (logger_default_set(NULL, NULL) != -ECHILD)
			_exit(10);
		if (logger_process_init() != 0)
			_exit(11);
		/* 不再访问继承的 a；新域中没有默认借用。 */
		absent();
		logger_t *b = make_logger("b.log", 0);

		CHECK(logger_default_set(NULL, b) == 0);
		LOG_INFO("CHILD_ONLY");
		CHECK(logger_default_set(b, NULL) == 0);
		CHECK(logger_destroy(&b) == 0);
		_exit(0);
	}
	int status;

	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	LOG_INFO("PARENT_AFTER");
	CHECK(logger_default_set(a, NULL) == 0);
	CHECK(logger_destroy(&a) == 0);
	CHECK(lines("a.log") == 2 && lines("b.log") == 1);
	CHECK(!file_contains("b.log", "PARENT_BEFORE"));
}

static _Atomic unsigned int accepted;
static _Atomic int start_readers;

static void *stress_reader(void *unused)
{
	(void)unused;
	wait_flag(&start_readers);
	for (unsigned int i = 0; i < 400; ++i) {
		errno = EDOM;
		int rc = LOG_INFO("CONCURRENT_RECORD_%u", i);

		CHECK(errno == EDOM);
		if (!rc)
			atomic_fetch_add(&accepted, 1);
		else
			CHECK(rc == -EAGAIN || rc == -ESHUTDOWN);
	}
	return NULL;
}

static void stress(void)
{
	logger_t *a = make_logger("a.log", 0);
	logger_t *b = make_logger("b.log", 0);
	logger_t *active = a;
	pthread_t readers[3];

	CHECK(logger_default_set(NULL, a) == 0);
	for (unsigned int i = 0; i < 3; ++i)
		CHECK(pthread_create(&readers[i], NULL, stress_reader, NULL) ==
		      0);
	atomic_store(&start_readers, 1);
	for (unsigned int i = 0; i < 200; ++i) {
		logger_t *next = active == a ? b : a;

		CHECK(logger_default_set(active, next) == 0);
		active = next;
	}
	for (unsigned int i = 0; i < 3; ++i)
		CHECK(pthread_join(readers[i], NULL) == 0);
	CHECK(logger_default_set(active, NULL) == 0);
	CHECK(logger_destroy(&a) == 0 && logger_destroy(&b) == 0);
	CHECK(lines("a.log") + lines("b.log") == atomic_load(&accepted));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	(void)alarm(20);
	char path[] = "/tmp/logger-default-XXXXXX";

	enter_temp(path);
	if (!strcmp(argv[1], "cold")) {
		errno = EDOM;
		CHECK(logger_default_set(NULL, NULL) == -ENODEV);
		CHECK(errno == EDOM);
		absent();
	}
	CHECK(logger_process_init() == 0);
	if (!strcmp(argv[1], "io"))
		io_failure();
	else if (!strcmp(argv[1], "fork"))
		fork_case();
	else if (!strcmp(argv[1], "stress"))
		stress();
	else
		healthy(!strcmp(argv[1], "async"));
	const char *files[] = { "a.log", "b.log", "a.log.logger.lock",
				"b.log.logger.lock" };
	for (unsigned int i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
		CHECK(unlink(files[i]) == 0 || errno == ENOENT);
	CHECK(chdir("/") == 0 && rmdir(path) == 0);
	printf("DEFAULT_BINDING_PASS=%s\n", argv[1]);
	return 0;
}
