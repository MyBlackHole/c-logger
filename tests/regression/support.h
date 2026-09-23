#ifndef LOGGER_REGRESSION_SUPPORT_H
#define LOGGER_REGRESSION_SUPPORT_H
#include <errno.h>
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
	while (nanosleep(&ts, &ts) && errno == EINTR) {
	}
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

static inline off_t file_size(const char *path)
{
	struct stat st;
	CHECK(stat(path, &st) == 0);
	return st.st_size;
}

static inline int file_contains(const char *path, const char *needle)
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
	/* The backend never unlinks a live coordination inode. Test owner has
     * already destroyed its logger; remove the persistent fixture here. */
	CHECK(unlink("out.log.logger.lock") == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(path) == 0);
}
#endif
