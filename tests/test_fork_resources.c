#define _GNU_SOURCE
#include "logger.h"
#include "regression/support.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/wait.h>

/* 公开 API 黑盒：枚举父方真实 fd，不连接私有符号或包装 DSO 内部调用。 */
int main(void)
{
	char path[] = "/tmp/logger-public-fork-fd-XXXXXX";
	enter_temp(path);
	CHECK(!logger_process_init());
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 8;
	logger_t *l = logger_create(&cfg);
	CHECK(l);
	int owned[4];
	unsigned count = 0;
	DIR *scan = opendir("/proc/self/fd");
	CHECK(scan);
	struct dirent *entry;
	while ((entry = readdir(scan))) {
		char *end;
		long number = strtol(entry->d_name, &end, 10);
		if (*end || number < 0 || number > INT_MAX)
			continue;
		char link[128], target[4096];
		int n = snprintf(link, sizeof(link), "/proc/self/fd/%ld",
				 number);
		CHECK(n > 0 && (size_t)n < sizeof(link));
		ssize_t length = readlink(link, target, sizeof(target) - 1);
		if (length < 0)
			continue;
		target[length] = 0;
		if (strncmp(target, path, strlen(path)))
			continue;
		CHECK(count < 4);
		owned[count++] = (int)number;
	}
	CHECK(!closedir(scan));
	CHECK(count == 3);
	int ready[2], finish[2];
	CHECK(!pipe(ready) && !pipe(finish));
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		for (unsigned i = 0; i < count; ++i) {
			if (fcntl(owned[i], F_GETFD) != -1 || errno != EBADF)
				_exit(10);
		}
		if (logger_process_init() != 0)
			_exit(11);
		char c = 1;
		if (write(ready[1], &c, 1) != 1 || read(finish[0], &c, 1) != 1)
			_exit(12);
		_exit(0);
	}
	char c = 1;
	CHECK(read(ready[0], &c, 1) == 1);
	for (unsigned i = 0; i < count; ++i)
		CHECK(fcntl(owned[i], F_GETFD) >= 0);
	int lock = open("out.log.logger.lock", O_RDWR | O_CLOEXEC);
	CHECK(lock >= 0);
	CHECK(flock(lock, LOCK_EX | LOCK_NB) == -1);
	CHECK(errno == EWOULDBLOCK || errno == EAGAIN);
	LOGGER_INFO(l, "fork", "PUBLIC_PARENT_CONTINUES");
	CHECK(!logger_destroy_status(l));
	CHECK(file_contains("out.log", "PUBLIC_PARENT_CONTINUES"));
	/* 子方尚在等待，父方释放后锁已可用。 */
	CHECK(!flock(lock, LOCK_EX | LOCK_NB));
	CHECK(write(finish[1], &c, 1) == 1);
	int status = 0;
	pid_t got;
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid && WIFEXITED(status) && !WEXITSTATUS(status));
	CHECK(!close(lock));
	for (unsigned i = 0; i < 2; ++i) {
		CHECK(!close(ready[i]));
		CHECK(!close(finish[i]));
	}
	leave_temp(path);
	puts("FORK_FD_PUBLIC_PASS=1");
	return 0;
}
