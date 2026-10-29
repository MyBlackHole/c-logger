#define _GNU_SOURCE
#include <errno.h>
#include "default_fixture.h"
static logger_t *default_owner;
#include "logger.h"
#include "support.h"
#include <fcntl.h>
#include <locale.h>
#include <wchar.h>

#define MESSAGE_CAPACITY 4095u
static const char marker[] = " [truncated]";

static void emit(logger_t *log, const char *mode, const char *text)
{
	errno = EDOM;
	if (!strcmp(mode, "global") || !strcmp(mode, "bootstrap"))
		CHECK(logger_global_write(LOGGER_INFO, "truncation", NULL, 0,
					  NULL, "%s", text) == 0);
	else
		CHECK(logger_log(log, LOGGER_INFO, "truncation", NULL, 0, NULL,
				 "%s", text) == 0);
	CHECK(errno == EDOM);
}

static void flush(logger_t *log, const char *mode)
{
	if (!strcmp(mode, "global"))
		CHECK(logger_default_flush() == 0);
	else if (strcmp(mode, "bootstrap"))
		CHECK(logger_flush(log) == 0);
}

int main(int argc, char **argv)
{
	CHECK(logger_process_init() == 0);
	CHECK(argc == 2);
	const char *mode = argv[1];
	CHECK(!strcmp(mode, "explicit-sync") ||
	      !strcmp(mode, "explicit-async") || !strcmp(mode, "global") ||
	      !strcmp(mode, "bootstrap") || !strcmp(mode, "status") ||
	      !strcmp(mode, "metadata-sync") ||
	      !strcmp(mode, "metadata-async"));
	char dir[] = "/tmp/logger-truncation-XXXXXX";
	enter_temp(dir);
	CHECK(setlocale(LC_CTYPE, "C") != NULL);
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "message.log";
	c.detail = !strncmp(mode, "metadata-", 9) ? LOGGER_DETAIL_DEBUG :
						    LOGGER_DETAIL_MINIMAL;
	c.include_source = 1;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = !strcmp(mode, "explicit-async") ||
		       !strcmp(mode, "global") ||
		       !strcmp(mode, "metadata-async");
	logger_t *log = NULL;
	int saved_stderr = -1;
	if (!strcmp(mode, "bootstrap")) {
		/* 无默认实例不再提供启动回退：必须在读取变参前拒绝。 */
		int touched = 7;
		errno = EDOM;
		CHECK(LOG_INFO("%8192s%n", "", &touched) == -ENODEV);
		CHECK(errno == EDOM && touched == 7);
		CHECK(access("message.log", F_OK) != 0);
		CHECK(chdir("/") == 0 && rmdir(dir) == 0);
		return 0;
	} else if (!strcmp(mode, "global")) {
		CHECK(fixture_default_create(&c, &default_owner) == 0);
	} else {
		CHECK(logger_create(&c, &log) == 0);
		CHECK(log != NULL);
	}

	const size_t lengths[] = { 0, 4, MESSAGE_CAPACITY, MESSAGE_CAPACITY + 1,
				   8191 };
	char text[8192];
	char line[8192];
	FILE *file = fopen("message.log", "r");
	CHECK(file != NULL);
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		size_t n = lengths[i];
		memset(text, 'x', n);
		text[n] = '\0';
		if (!strcmp(mode, "status")) {
			int rc = logger_log_sync_status(log, LOGGER_INFO,
							"truncation", NULL, 0,
							NULL, "%s", text);
			CHECK(rc == (n > MESSAGE_CAPACITY ? -EOVERFLOW : 0));
			if (n > MESSAGE_CAPACITY)
				continue;
		} else {
			emit(log, mode, text);
		}
		flush(log, mode);
		clearerr(file);
		CHECK(fgets(line, sizeof(line), file) != NULL);
		size_t stored = n > MESSAGE_CAPACITY ? MESSAGE_CAPACITY : n;
		if (n > MESSAGE_CAPACITY)
			memcpy(text + MESSAGE_CAPACITY - (sizeof(marker) - 1),
			       marker, sizeof(marker));
		size_t total = strlen(line);
		CHECK(total >= stored + 1);
		CHECK(line[total - 1] == '\n');
		CHECK(!memcmp(line + total - stored - 1, text, stored));
		if (n <= MESSAGE_CAPACITY)
			CHECK(strstr(line, marker) == NULL);
	}
	if (!strncmp(mode, "metadata-", 9)) {
		memset(text, 'm', sizeof(text) - 1);
		text[sizeof(text) - 1] = '\0';
		logger_log(log, LOGGER_INFO, text, text, 1, text, "small-body");
		flush(log, mode);
		clearerr(file);
		CHECK(fgets(line, sizeof(line), file) != NULL);
		if (!strcmp(mode, "metadata-sync")) {
			size_t n = strlen(line);
			CHECK(n == 5119 && line[n - 1] == '\n');
			CHECK(!memcmp(line + n - sizeof(marker), marker,
				      sizeof(marker) - 1));
		} else {
			CHECK(strstr(line, "~") != NULL);
			CHECK(strstr(line, "small-body\n") != NULL);
		}
		/* 仅来源信息也能耗尽同步行容量。 */
		logger_log(log, LOGGER_INFO, "short", text, 1, text,
			   "small-body");
		flush(log, mode);
		clearerr(file);
		CHECK(fgets(line, sizeof(line), file) != NULL);
		if (!strcmp(mode, "metadata-sync")) {
			size_t n = strlen(line);
			CHECK(n == 5119 && line[n - 1] == '\n');
			CHECK(!memcmp(line + n - sizeof(marker), marker,
				      sizeof(marker) - 1));
		} else {
			CHECK(strstr(line, "~") != NULL);
			CHECK(strstr(line, "small-body\n") != NULL);
		}
		/* 带确认路径必须拒绝元数据导致的行溢出。 */
		CHECK(logger_log_sync_status(log, LOGGER_INFO, text, text, 1,
					     text, "small-body") == -EOVERFLOW);
	}
	/* C 区域设置无法编码此代理码点：格式化失败不输出任何内容。 */
	errno = EDOM;
	if (!strcmp(mode, "global") || !strcmp(mode, "bootstrap"))
		CHECK(logger_global_write(LOGGER_INFO, "truncation", NULL, 0,
					  NULL, "%lc",
					  (wint_t)0xd800) == -EILSEQ);
	else if (!strcmp(mode, "status"))
		CHECK(logger_log_sync_status(log, LOGGER_INFO, "truncation",
					     NULL, 0, NULL, "%lc",
					     (wint_t)0xd800) == -EILSEQ);
	else
		CHECK(logger_log(log, LOGGER_INFO, "truncation", NULL, 0, NULL,
				 "%lc", (wint_t)0xd800) == -EILSEQ);
	CHECK(errno == EDOM);
	flush(log, mode);
	clearerr(file);
	CHECK(fgetc(file) == EOF && !ferror(file));
	CHECK(fclose(file) == 0);
	if (strcmp(mode, "bootstrap")) {
		logger_io_metrics_t io;
		if (!strcmp(mode, "global"))
			CHECK(logger_get_global_io_metrics(&io) == 0);
		else
			CHECK(logger_get_io_metrics(log, &io) == 0);
		uint64_t expected = !strcmp(mode, "status")	   ? 3 :
				    !strncmp(mode, "metadata-", 9) ? 7 :
								     5;
		CHECK(io.emitted_records == expected);
		CHECK(io.failed_records == 0 && io.first_error == 0);
		CHECK(io.async_completed + io.sync_completed == expected);
	}
	if (!strcmp(mode, "global"))
		CHECK(fixture_default_destroy(&default_owner) == 0);
	else if (strcmp(mode, "bootstrap"))
		CHECK(logger_destroy(&log) == 0);
	if (saved_stderr >= 0) {
		CHECK(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
		CHECK(close(saved_stderr) == 0);
	}
	CHECK(unlink("message.log") == 0);
	if (strcmp(mode, "bootstrap"))
		CHECK(unlink("message.log.logger.lock") == 0);
	CHECK(chdir("/") == 0);
	CHECK(rmdir(dir) == 0);
	return 0;
}
