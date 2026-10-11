#define _GNU_SOURCE
#include "logger.h"
#include "support.h"
#include <fcntl.h>
#include <locale.h>
#include <wchar.h>

#define MESSAGE_CAPACITY 4095u
static const char marker[] = " [truncated]";

static void emit(logger_t *log, const char *mode, const char *text)
{
	if (!strcmp(mode, "global") || !strcmp(mode, "bootstrap"))
		logger_global_write(LOGGER_INFO, "truncation", NULL, 0, NULL,
				    "%s", text);
	else
		logger_log(log, LOGGER_INFO, "truncation", NULL, 0, NULL,
			   "%s", text);
}

static void flush(logger_t *log, const char *mode)
{
	if (!strcmp(mode, "global"))
		CHECK(logger_flush_status() == 0);
	else if (strcmp(mode, "bootstrap"))
		CHECK(logger_flush_instance_status(log) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	const char *mode = argv[1];
	CHECK(!strcmp(mode, "explicit-sync") || !strcmp(mode, "explicit-async") ||
	      !strcmp(mode, "global") || !strcmp(mode, "bootstrap") ||
	      !strcmp(mode, "status") || !strcmp(mode, "metadata-sync") ||
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
	c.async_mode = !strcmp(mode, "explicit-async") || !strcmp(mode, "global") ||
		!strcmp(mode, "metadata-async");
	logger_t *log = NULL;
	int saved_stderr = -1;
	if (!strcmp(mode, "bootstrap")) {
		int fd = open("message.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
		CHECK(fd >= 0);
		saved_stderr = dup(STDERR_FILENO);
		CHECK(saved_stderr >= 0);
		CHECK(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
		CHECK(close(fd) == 0);
	} else if (!strcmp(mode, "global")) {
		CHECK(logger_init(&c) == 0);
	} else {
		log = logger_create(&c);
		CHECK(log != NULL);
	}

	const size_t lengths[] = {0, 4, MESSAGE_CAPACITY, MESSAGE_CAPACITY + 1, 8191};
	char text[8192];
	char line[8192];
	FILE *file = fopen("message.log", "r");
	CHECK(file != NULL);
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		size_t n = lengths[i];
		memset(text, 'x', n);
		text[n] = '\0';
		if (!strcmp(mode, "status")) {
			int rc = logger_log_sync_status(log, LOGGER_INFO, "truncation",
						NULL, 0, NULL, "%s", text);
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
		/* Source alone can exhaust the synchronous line too. */
		logger_log(log, LOGGER_INFO, "short", text, 1, text, "small-body");
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
		/* The acknowledged path must reject metadata-induced line overflow. */
		CHECK(logger_log_sync_status(log, LOGGER_INFO, text, text, 1, text,
					    "small-body") == -EOVERFLOW);
	}
	/* C locale cannot encode this surrogate: formatting failure emits nothing. */
	if (!strcmp(mode, "global") || !strcmp(mode, "bootstrap"))
		logger_global_write(LOGGER_INFO, "truncation", NULL, 0, NULL,
				    "%lc", (wint_t)0xd800);
	else if (!strcmp(mode, "status"))
		CHECK(logger_log_sync_status(log, LOGGER_INFO, "truncation", NULL,
					    0, NULL, "%lc", (wint_t)0xd800) == -EILSEQ);
	else
		logger_log(log, LOGGER_INFO, "truncation", NULL, 0, NULL,
			   "%lc", (wint_t)0xd800);
	flush(log, mode);
	clearerr(file);
	CHECK(fgetc(file) == EOF && !ferror(file));
	CHECK(fclose(file) == 0);
	if (strcmp(mode, "bootstrap")) {
		logger_io_metrics_t io;
		if (!strcmp(mode, "global"))
			logger_get_global_io_metrics(&io);
		else
			logger_get_io_metrics(log, &io);
		uint64_t expected = !strcmp(mode, "status") ? 3 :
			!strncmp(mode, "metadata-", 9) ? 7 : 5;
		CHECK(io.emitted_records == expected);
		CHECK(io.failed_records == 0 && io.first_error == 0);
		CHECK(io.async_completed + io.sync_completed == expected);
	}
	if (!strcmp(mode, "global"))
		CHECK(logger_shutdown_status() == 0);
	else if (strcmp(mode, "bootstrap"))
		CHECK(logger_destroy_status(log) == 0);
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
