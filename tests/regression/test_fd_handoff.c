#define _GNU_SOURCE
#include "logger_fd.h"
#include "support.h"

#ifndef LOGGER_TEST_HANDOFF_VARIANT
#error "LOGGER_TEST_HANDOFF_VARIANT is required for this test-only driver"
#endif
#if LOGGER_TEST_HANDOFF_VARIANT < 0 || LOGGER_TEST_HANDOFF_VARIANT > 2
#error "invalid LOGGER_TEST_HANDOFF_VARIANT"
#endif

/* 健康目标直接包含未改写的生产实现。两个错误目标仅在选定移交时，
 * 以同型复制模拟忘记消费来源；不会把错误实现写回生产源码或归档。
 */
#if LOGGER_TEST_HANDOFF_VARIANT != 0
static int copy_active;
static unsigned int copy_calls;

static struct logger_owned_fd copy_or_move(struct logger_owned_fd *owner)
{
	if (copy_active) {
		++copy_calls;
		return *owner;
	}
	return logger_fd_move(owner);
}
#define logger_fd_move copy_or_move
#endif
#include "../../src/logger_file.c"
#if LOGGER_TEST_HANDOFF_VARIANT != 0
#undef logger_fd_move
#endif

/* 固定机器标记与退出码供外层判定器校验，不把任意非零当成反例命中。 */
static int result(const char *stage, int code)
{
	int written = printf("FD_HANDOFF_VARIANT=%d\nFD_HANDOFF_RESULT=%s\n",
			     LOGGER_TEST_HANDOFF_VARIANT, stage);

	if (written < 0 || fflush(stdout))
		return 1;
	return code;
}

int main(void)
{
	struct logger_file file = LOGGER_FILE_EMPTY;
	logger_rotation_config_t rotation = { .mode = LOGGER_ROTATE_NONE };
	struct logger_owned_fd candidate = logger_fd_own(-1);
	struct stat st;

	CHECK(logger_process_init() == 0);
	CHECK(!logger_file_reserve(&file, "handoff.log", rotation, 0600));
#if LOGGER_TEST_HANDOFF_VARIANT == 1
	copy_active = 1;
#endif
	CHECK(!open_candidate(&file, 0, &candidate, &st));
#if LOGGER_TEST_HANDOFF_VARIANT == 1
	copy_active = 0;
	CHECK(copy_calls == 1);
#endif
	CHECK(logger_fd_valid(&candidate));
	int raw = logger_fd_number(logger_fd_borrow(&candidate));
	int valid = fcntl(raw, F_GETFD);

	if (valid < 0) {
		CHECK(errno == EBADF);
		/* 错误夹具仅在独立进程退出，避免继续使用已失效候选。 */
		return result("output_source_not_consumed", 41);
	}
#if LOGGER_TEST_HANDOFF_VARIANT == 2
	copy_active = 1;
#endif
	install_fd(&file, &candidate, &st, 0);
#if LOGGER_TEST_HANDOFF_VARIANT == 2
	copy_active = 0;
	CHECK(copy_calls == 1);
#endif
	if (logger_fd_valid(&candidate))
		return result("install_source_not_consumed", 42);
	logger_fd_cleanup(&candidate);
	CHECK(file_data_fd(&file) == raw && fcntl(raw, F_GETFD) >= 0);

	struct timespec now;
	static const char text[] = "handoff-record\n";

	CHECK(!clock_gettime(CLOCK_REALTIME, &now));
	CHECK(!logger_file_write(&file, text, sizeof(text) - 1, &now, 1));
	int lockfd = file_lock_fd(&file);
	int dirfd = file_dir_fd(&file);

	CHECK(!logger_file_close_status(&file));
	CHECK(!logger_file_close_status(&file));
	CHECK(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(lockfd, F_GETFD) == -1 && errno == EBADF);
	CHECK(fcntl(dirfd, F_GETFD) == -1 && errno == EBADF);
	int fd = open("handoff.log", O_RDONLY | O_CLOEXEC);
	char contents[sizeof(text)];

	CHECK(fd >= 0);
	CHECK(read(fd, contents, sizeof(contents)) == sizeof(text) - 1);
	CHECK(!memcmp(contents, text, sizeof(text) - 1));
	CHECK(!close(fd));
	CHECK(!unlink("handoff.log"));
	CHECK(!unlink("handoff.log.logger.lock"));
	return result("ok", 0);
}
