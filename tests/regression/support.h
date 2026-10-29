#ifndef LOGGER_REGRESSION_SUPPORT_H
#define LOGGER_REGRESSION_SUPPORT_H
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                            \
	do {                                                                \
		if (!(x)) {                                                 \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, \
				__LINE__, #x, errno);                       \
			exit(1);                                            \
		}                                                           \
	} while (0)

static inline void nap_ms(void)
{
	struct timespec ts = { 0, 1000000 };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static inline void wait_flag(const _Atomic int *flag)
{
	for (int i = 0; i < 5000; ++i) {
		if (atomic_load_explicit(flag, memory_order_acquire))
			return;
		nap_ms();
	}
	CHECK(!"timed out waiting for test synchronization point");
}

static inline void enter_temp(char *path)
{
	CHECK(mkdtemp(path) != NULL);
	CHECK(chdir(path) == 0);
}

static inline void enter_temp_workspace(char *path, size_t size,
					const char *name)
{
	char root[PATH_MAX];
	const char *configured = getenv("TMPDIR");

	if (!configured || !*configured) {
		CHECK(getcwd(root, sizeof(root)) != NULL);
	} else if (configured[0] == '/') {
		int length = snprintf(root, sizeof(root), "%s", configured);

		CHECK(length > 0 && (size_t)length < sizeof(root));
	} else {
		char cwd[PATH_MAX];

		CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
		int length =
			snprintf(root, sizeof(root), "%s/%s", cwd, configured);
		CHECK(length > 0 && (size_t)length < sizeof(root));
	}
	int length = snprintf(path, size, "%s/%s-XXXXXX", root, name);

	CHECK(length > 0 && (size_t)length < size);
	enter_temp(path);
}

static inline off_t file_size(const char *path)
{
	struct stat st;

	CHECK(stat(path, &st) == 0);
	return st.st_size;
}

static inline bool file_contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "rb");

	CHECK(f != NULL);
	char text[32768];
	size_t n = fread(text, 1, sizeof(text) - 1, f);

	CHECK(!ferror(f));
	text[n] = '\0';
	CHECK(fclose(f) == 0);
	return strstr(text, needle) != NULL;
}

static inline void leave_temp(char *path)
{
	CHECK(unlink("out.log") == 0);
	/*
	 * 后端绝不删除存活的协调 inode。测试所有者已销毁其 logger；
	 * 在此移除持久化测试夹具。
	 */
	CHECK(unlink("out.log.logger.lock") == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(path) == 0);
}
#endif /* LOGGER_REGRESSION_SUPPORT_H */
