#define _GNU_SOURCE
#include <stdbool.h>
#include "crash_support.h"
#include "logger.h"
#include "regression/support.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>

#define ACTIVE "crash.log"
#define LOCK ACTIVE ".logger.lock"
#define BASELINE_RECORDS 10u
#define TARGET_SEQUENCE BASELINE_RECORDS
#define RECORD_CAPACITY 512u

static void verify_records(const char *point, unsigned int recovered,
			   int baseline_only);

static const char *cut_point;
static int armed;
static int active_switched;
static const char *const points[] = { "acknowledged",
				      "before_file_fsync",
				      "after_file_fsync",
				      "rotation_after_file_fsync",
				      "file_after_archive_rename",
				      "file_after_archive_dirsync",
				      "file_after_active_open",
				      "file_after_active_dirsync" };

bool crash_fixture_point_valid(const char *point)
{
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i) {
		if (!strcmp(point, points[i]))
			return true;
	}
	return false;
}

static bool rotates(const char *point)
{
	return !strncmp(point, "file_after_", 11) ||
	       !strcmp(point, "rotation_after_file_fsync");
}

/*
 * 固定测试时钟允许对整条格式化日志逐字节检查，包括其元数据。
 * 真实单调时钟与超时保持不变。
 */
int __real_clock_gettime(clockid_t clock, struct timespec *value);

int __wrap_clock_gettime(clockid_t clock, struct timespec *value)
{
	if (clock == CLOCK_REALTIME) {
		*value = (struct timespec){ 1700000000, 123456000 };
		return 0;
	}
	return __real_clock_gettime(clock, value);
}

static size_t payload(unsigned int sequence, char *out, size_t capacity)
{
	int n = snprintf(
		out, capacity,
		"LOGGER_CRASH_V1 seq=%03u payload="
		"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"/full-content-must-survive/0123456789abcdefghijklmnopqrstuvwxyz"
		" end=%03u",
		sequence, sequence);
	CHECK(n > 0 && (size_t)n < capacity);
	return (size_t)n;
}

static size_t record_line(unsigned int sequence, char *out, size_t capacity)
{
	char body[RECORD_CAPACITY];

	payload(sequence, body, sizeof(body));
	int n = snprintf(out, capacity,
			 "2023-11-14T22:13:20.123456+0000 INFO  %s\n", body);
	CHECK(n > 0 && (size_t)n < capacity);
	return (size_t)n;
}

void __real_logger_fault_crash_if_requested(const char *point);

void __wrap_logger_fault_crash_if_requested(const char *point)
{
	if (armed) {
		if (!strcmp(point, "file_after_active_dirsync"))
			active_switched = 1;
		if (!strcmp(point, cut_point))
			crash_fixture_cut(point);
	}
	__real_logger_fault_crash_if_requested(point);
}

int __real_fsync(int fd);

int __wrap_fsync(int fd)
{
	int match = 0;

	if (armed && (!strcmp(cut_point, "before_file_fsync") ||
		      !strcmp(cut_point, "after_file_fsync") ||
		      (!strcmp(cut_point, "rotation_after_file_fsync") &&
		       active_switched))) {
		struct stat data;
		struct stat active;

		CHECK(fstat(fd, &data) == 0);
		CHECK(lstat(ACTIVE, &active) == 0);
		char line[RECORD_CAPACITY];
		size_t size = record_line(TARGET_SEQUENCE, line, sizeof(line));

		size *= rotates(cut_point) ? 1u : BASELINE_RECORDS + 1u;
		/*
		 * 绝不把目录 fsync、已退役归档或轮转中新空 inode 的 fsync
		 * 计为目标记录的数据 fsync。
		 */
		match = S_ISREG(data.st_mode) && data.st_nlink == 1 &&
			data.st_dev == active.st_dev &&
			data.st_ino == active.st_ino &&
			data.st_size == (off_t)size;
	}
	if (match && !strcmp(cut_point, "before_file_fsync"))
		crash_fixture_cut(cut_point);
	int result = __real_fsync(fd);

	if (!result && match && strcmp(cut_point, "before_file_fsync"))
		crash_fixture_cut(cut_point);
	return result;
}

static logger_config_t config(int rotation)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();

	c.outputs = LOGGER_OUT_FILE;
	c.file_path = ACTIVE;
	c.async_mode = 0;
	c.detail = LOGGER_DETAIL_MINIMAL;
	c.rotation.mode = rotation ? LOGGER_ROTATE_SIZE : LOGGER_ROTATE_NONE;
	c.rotation.max_file_size = 1; /* 每个归档恰好一条完整记录 */
	c.rotation.retention_days = 0; /* 绝不使已确认基线过期 */
	return c;
}

static void initialize(void)
{
	CHECK(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	/* 继承的故障设置不得满足或改变此契约。 */
	CHECK(unsetenv("LOGGER_CRASH_POINT") == 0);
	CHECK(unsetenv("LOGGER_FAULT_POINT") == 0);
	CHECK(unsetenv("LOGGER_FAULT_SHORT_WRITE") == 0);
}

static void emit(logger_t *logger, unsigned int sequence)
{
	char body[RECORD_CAPACITY];

	payload(sequence, body, sizeof(body));
	CHECK(logger_log_sync_status(logger, LOGGER_INFO, "crash", __FILE__,
				     __LINE__, __func__, "%s", body) == 0);
}

void crash_fixture_write(const char *point)
{
	CHECK(crash_fixture_point_valid(point));
	initialize();
	cut_point = point;
	logger_config_t c = config(rotates(point));
	logger_t *logger = NULL;

	CHECK(logger_create(&c, &logger) == 0);
	CHECK(logger != NULL);
	for (unsigned int i = 0; i < BASELINE_RECORDS; ++i)
		emit(logger, i);
	CHECK(logger_flush(logger) == 0);
	verify_records(point, 0, 1);
	/*
	 * 强制终止前不进行优雅关闭：控制方只在基线成功写入并刷新确认后
	 * 才收到切断标记。
	 */
	if (!strcmp(point, "acknowledged"))
		crash_fixture_cut(point);
	armed = 1;
	emit(logger, TARGET_SEQUENCE);
	CHECK(!"requested ordinary Logger cut point was not reached");
}

static bool archive_name(const char *name)
{
	if (strlen(name) != 33 || strncmp(name, "crash.", 6) ||
	    strcmp(name + 29, ".log"))
		return false;
	const char *stamp = name + 6;

	for (unsigned int i = 0; i < 23; ++i) {
		char delimiter = i == 8	 ? 'T' :
				 i == 15 ? '.' :
				 i == 22 ? 'Z' :
					   0;
		if (delimiter ? stamp[i] != delimiter :
				stamp[i] < '0' || stamp[i] > '9')
			return false;
	}
	return true;
}

static int select_archive(const struct dirent *entry)
{
	return archive_name(entry->d_name);
}

static size_t read_segment(const char *name, char *out, size_t capacity)
{
	int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	CHECK(fd >= 0);
	struct stat st;

	CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1);
	CHECK(st.st_size >= 0 && (uintmax_t)st.st_size < capacity);
	size_t size = (size_t)st.st_size;
	size_t offset = 0;

	while (offset < size) {
		ssize_t n = read(fd, out + offset, size - offset);

		if (n < 0 && errno == EINTR)
			continue;
		CHECK(n > 0);
		offset += (size_t)n;
	}
	char extra;

	CHECK(read(fd, &extra, 1) == 0 && close(fd) == 0);
	return size;
}

static void verify_records(const char *point, unsigned int recovered,
			   int baseline_only)
{
	CHECK(crash_fixture_point_valid(point) && recovered <= 2);
	char bytes[8192];
	char line[RECORD_CAPACITY];
	size_t used = 0;
	size_t length = record_line(0, line, sizeof(line));
	DIR *directory = opendir(".");

	CHECK(directory);
	struct dirent *entry;
	int active_present = 0;

	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			CHECK(errno == 0);
			break;
		}
		if (!strcmp(entry->d_name, ACTIVE))
			active_present = 1;
		else if (strncmp(entry->d_name, "crash.", 6) == 0)
			CHECK(!strcmp(entry->d_name, LOCK) ||
			      archive_name(entry->d_name));
	}
	CHECK(closedir(directory) == 0);
	struct dirent **archives;
	int count = scandir(".", &archives, select_archive, alphasort);

	CHECK(count >= 0);
	CHECK(rotates(point) ? count >= 9 && count <= 10 : count == 0);
	if (!baseline_only && rotates(point) &&
	    strcmp(point, "file_after_archive_rename"))
		CHECK(count == 10); /* 归档目录 fsync 已完成 */
	if (!baseline_only && !strcmp(point, "file_after_active_dirsync"))
		CHECK(active_present); /* 新活动 inode 与目录已持久化 */
	for (int i = 0; i < count; ++i) {
		/*
		 * 对严格且唯一的 UTC 名称排序可重建串行写者的顺序。
		 * 单记录分段绝不能包含部分记录或额外记录。
		 */
		size_t size = read_segment(archives[i]->d_name, bytes + used,
					   sizeof(bytes) - used);
		CHECK(size == length);
		used += size;
		free(archives[i]);
	}
	free(archives);
	if (active_present)
		used += read_segment(ACTIVE, bytes + used,
				     sizeof(bytes) - used);
	CHECK(!recovered || active_present);
	size_t offset = 0;

	for (unsigned int i = 0; i < BASELINE_RECORDS; ++i) {
		length = record_line(i, line, sizeof(line));
		CHECK(used - offset >= length &&
		      !memcmp(bytes + offset, line, length));
		offset += length;
	}
	/*
	 * 被中断记录未经确认。fsync 前只允许记录缺失或为此精确目标的字节前缀。
	 * 不接受任意垃圾、重复基线或无关尾部。
	 * 读取原始字节而不是行：撕裂尾部不一定以换行符结束。
	 */
	size_t recovery_bytes = recovered * length;

	CHECK(used - offset >= recovery_bytes);
	size_t target_bytes = used - offset - recovery_bytes;

	length = record_line(TARGET_SEQUENCE, line, sizeof(line));
	if (baseline_only) {
		CHECK(target_bytes == 0);
	} else if (!strcmp(point, "before_file_fsync")) {
		CHECK(target_bytes <= length &&
		      !memcmp(bytes + offset, line, target_bytes));
	} else if (!strcmp(point, "after_file_fsync") ||
		   !strcmp(point, "rotation_after_file_fsync")) {
		CHECK(target_bytes == length &&
		      !memcmp(bytes + offset, line, length));
	} else {
		CHECK(target_bytes == 0);
	}
	offset += target_bytes;
	for (unsigned int i = 0; i < recovered; ++i) {
		length = record_line(BASELINE_RECORDS + 1u + i, line,
				     sizeof(line));
		CHECK(!memcmp(bytes + offset, line, length));
		offset += length;
	}
	CHECK(offset == used);
	printf("LOGGER_CRASH_VERIFY point=%s baseline=10 recovery=%u archives=%d target_bytes=%zu\n",
	       point, recovered, count, target_bytes);
}

void crash_fixture_verify(const char *point, unsigned int recovered)
{
	verify_records(point, recovered, 0);
}

void crash_fixture_recover(const char *point)
{
	initialize();
	/*
	 * 初次恢复时也要求存在 fsync 后的目标；应用未收到其返回，
	 * 但精确的数据 fsync 已成功。
	 */
	crash_fixture_verify(point, 0);
	logger_config_t c = config(0);

	for (unsigned int i = 0; i < 2; ++i) {
		logger_t *logger = NULL;

		CHECK(logger_create(&c, &logger) == 0);
		CHECK(logger != NULL); /* 被终止的所有者与先前恢复已释放租约 */
		errno = 0;
		CHECK(logger_create(&c, &(logger_t *){ NULL }) == -EBUSY);
		emit(logger, BASELINE_RECORDS + 1u + i);
		CHECK(logger_flush(logger) == 0);
		CHECK(logger_destroy(&logger) == 0);
		crash_fixture_verify(point, i + 1u);
	}
}

void crash_fixture_cleanup(const char *directory)
{
	DIR *dir = opendir(".");

	CHECK(dir);
	struct dirent *entry;

	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			CHECK(!strcmp(entry->d_name, ACTIVE) ||
			      !strcmp(entry->d_name, LOCK) ||
			      archive_name(entry->d_name));
			CHECK(unlink(entry->d_name) == 0);
		}
	}
	CHECK(closedir(dir) == 0 && chdir("/") == 0 && rmdir(directory) == 0);
}
