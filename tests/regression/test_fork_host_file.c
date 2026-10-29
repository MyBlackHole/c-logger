#define _GNU_SOURCE
#include "logger.h"
#include "support.h"
#include <fcntl.h>
#include <sys/wait.h>

/*
 * 宿主FILE不由Logger登记、关闭或解锁。子方不使用继承的FILE*；
 * 只验证外部fd存活，并在新的独立运行域使用自己的日志实例。
 */
static FILE *foreign;
static _Atomic int foreign_locked;
static _Atomic int release_foreign;

static void *foreign_holder(void *unused)
{
	(void)unused;
	flockfile(foreign);
	atomic_store(&foreign_locked, 1);
	wait_flag(&release_foreign);
	funlockfile(foreign);
	return NULL;
}

static void child_expect(int valid, int code)
{
	if (!valid)
		_exit(code);
}

static logger_config_t file_config(const char *path)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = path;
	c.async_mode = 1;
	c.queue_capacity = 64;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	return c;
}

static void child_run(logger_t *old, int foreign_fd)
{
	child_expect(fcntl(foreign_fd, F_GETFD) >= 0, 20);
	errno = EDOM;
	int rc = LOGGER_INFO(old, "old", "FOREIGN_OLD_INVALID");
	child_expect(rc == -ECHILD && errno == EDOM, 21);
	child_expect(logger_process_init() == 0, 22);
	child_expect(fcntl(foreign_fd, F_GETFD) >= 0, 23);

	logger_config_t config = file_config("child.log");
	logger_t *child = NULL;
	child_expect(logger_create(&config, &child) == 0 && child, 24);
	child_expect(LOGGER_INFO(child, "child", "CHILD_START") == 0, 25);
	child_expect(LOGGER_INFO(child, "child", "CHILD_FINISH") == 0, 26);
	child_expect(logger_flush(child) == 0, 27);
	logger_io_metrics_t io;
	child_expect(logger_get_io_metrics(child, &io) == 0, 28);
	child_expect(io.emitted_records == 2 && io.failed_records == 0, 29);
	logger_metrics_t metrics;
	child_expect(logger_get_metrics(child, &metrics) == 0, 30);
	child_expect(metrics.dropped[LOGGER_INFO] == 0, 31);
	child_expect(logger_destroy(&child) == 0 && child == NULL, 32);
	child_expect(fcntl(foreign_fd, F_GETFD) >= 0, 33);
	_exit(0);
}

static void wait_child(pid_t pid)
{
	int status = 0;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		fprintf(stderr, "CHILD_STATUS=%d\n", status);
	CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned int count_lines(const char *path, const char *marker)
{
	FILE *stream = fopen(path, "r");
	CHECK(stream);
	char line[8192];
	unsigned int count = 0;
	while (fgets(line, sizeof(line), stream)) {
		CHECK(strstr(line, marker) != NULL);
		++count;
	}
	CHECK(!ferror(stream));
	CHECK(!fclose(stream));
	return count;
}

static void remove_log(const char *path)
{
	CHECK(!unlink(path));
	char lock[96];
	CHECK(snprintf(lock, sizeof(lock), "%s.logger.lock", path) > 0);
	CHECK(!unlink(lock));
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	int held = !strcmp(argv[1], "held");
	CHECK(held || !strcmp(argv[1], "quiesced"));
	alarm(40);
	char directory[] = "/tmp/logger-host-file-XXXXXX";
	enter_temp(directory);
	CHECK(logger_process_init() == 0);

	logger_config_t cfg = file_config("parent.log");
	logger_t *parent = NULL;
	CHECK(logger_create(&cfg, &parent) == 0 && parent);
	CHECK(LOGGER_INFO(parent, "parent", "PARENT_START") == 0);

	foreign = fopen("foreign.log", "w+");
	CHECK(foreign != NULL);
	int foreign_fd = fileno(foreign);
	CHECK(foreign_fd >= 0);
	pthread_t worker;
	CHECK(!pthread_create(&worker, NULL, foreign_holder, NULL));
	wait_flag(&foreign_locked);
	if (!held) {
		atomic_store(&release_foreign, 1);
		CHECK(!pthread_join(worker, NULL));
	}
	CHECK(fcntl(foreign_fd, F_GETFD) >= 0);
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid)
		child_run(parent, foreign_fd);
	wait_child(pid);

	/* held模式等待child真正结束后才放行宿主自己的FILE锁。 */
	if (held) {
		CHECK(!atomic_load(&release_foreign));
		atomic_store(&release_foreign, 1);
		CHECK(!pthread_join(worker, NULL));
	}
	CHECK(fcntl(foreign_fd, F_GETFD) >= 0);
	CHECK(fputs("FOREIGN_HOST_ONLY\n", foreign) >= 0);
	CHECK(!fflush(foreign));
	CHECK(!fclose(foreign));
	CHECK(LOGGER_INFO(parent, "parent", "PARENT_FINISH") == 0);
	CHECK(logger_flush(parent) == 0);
	logger_io_metrics_t io;
	CHECK(logger_get_io_metrics(parent, &io) == 0);
	CHECK(io.emitted_records == 2 && io.failed_records == 0);
	CHECK(logger_destroy(&parent) == 0 && parent == NULL);

	CHECK(count_lines("parent.log", "PARENT_") == 2);
	CHECK(count_lines("child.log", "CHILD_") == 2);
	CHECK(count_lines("foreign.log", "FOREIGN_HOST_ONLY") == 1);
	CHECK(!file_contains("parent.log", "CHILD_"));
	CHECK(!file_contains("child.log", "PARENT_"));
	CHECK(!file_contains("foreign.log", "CHILD_"));
	remove_log("parent.log");
	remove_log("child.log");
	CHECK(!unlink("foreign.log"));
	CHECK(!chdir("/"));
	CHECK(!rmdir(directory));
	printf("FORK_HOST_FILE_PASS=%s foreign_fd_open=1 "
	       "parent_records=2 child_records=2\n",
	       argv[1]);
	return 0;
}
