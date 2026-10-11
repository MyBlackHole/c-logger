#define _GNU_SOURCE
#include "logger_internal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test) do { \
	if (!(test)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
		exit(1); \
	} \
} while (0)

static int fail_format;
int __real_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int __wrap_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
	if (fail_format) {
		fail_format = 0;
		return -1;
	}
	return __real_vsnprintf(out, cap, fmt, ap);
}

/* glibc redirects fortified production calls to __vsnprintf_chk. Exercise the
 * same failure contract without disabling the library's FORTIFY protection. */
int __real___vsnprintf_chk(char *out, size_t cap, int flag, size_t object_size,
			  const char *fmt, va_list ap);
int __wrap___vsnprintf_chk(char *out, size_t cap, int flag, size_t object_size,
			  const char *fmt, va_list ap)
{
	if (fail_format) {
		fail_format = 0;
		return -1;
	}
	return __real___vsnprintf_chk(out, cap, flag, object_size, fmt, ap);
}

int main(void)
{
	CHECK(!setenv("TZ", "UTC0", 1));
	tzset();
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.async_mode = 0;
	cfg.outputs = LOGGER_OUT_STDERR;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&cfg);
	logger_message_t *m = calloc(1, sizeof(*m));
	char *out = malloc(LOGGER_LINE_MAX);
	char *expected = malloc(LOGGER_LINE_MAX);

	CHECK(l && m && out && expected);
	m->ts = (struct timespec){ 1700000000, 123456000 };
	m->level = LOGGER_INFO;
	m->module = "formatter";
	m->pid = getpid();
	m->tid = getpid();
	memcpy(m->text, "payload", sizeof("payload"));
	size_t length = 0;
	CHECK(!logger_format_line_checked(l, m, out, LOGGER_LINE_MAX, &length));
	CHECK(length > 1 && out[length - 1] == '\n');
	size_t valid = length;
	memcpy(expected, out, valid + 1);
	CHECK(logger_format_line(l, m, out, LOGGER_LINE_MAX) == valid);
	CHECK(!memcmp(out, expected, valid + 1));
	for (size_t cap = 0; cap <= valid; ++cap) {
		length = 999;
		CHECK(logger_format_line_checked(l, m, out, cap, &length) == -EOVERFLOW);
		CHECK(!length && (!cap || !out[0]));
		CHECK(logger_format_line(l, m, out, cap) < (cap ? cap : 1));
	}
	CHECK(!logger_format_line_checked(l, m, out, valid + 1, &length));
	CHECK(length == valid && !memcmp(out, expected, valid + 1));

	fail_format = 1;
	CHECK(logger_format_line_checked(l, m, out, LOGGER_LINE_MAX, &length) == -EILSEQ);
	CHECK(!fail_format && !length && !out[0]);
	CHECK(!logger_format_line_checked(l, m, out, LOGGER_LINE_MAX, &length));
	m->ts.tv_nsec = -1;
	CHECK(logger_format_line_checked(l, m, out, LOGGER_LINE_MAX, &length) == -EINVAL);
	CHECK(!length && !out[0]);
	m->ts.tv_nsec = 1000000000L;
	CHECK(logger_format_line_checked(l, m, out, LOGGER_LINE_MAX, &length) == -EINVAL);
	CHECK(!length && !out[0]);
	CHECK(!logger_destroy_status(l));
	free(expected);
	free(out);
	free(m);
	puts("checked formatter: capacities, parity, encoding failure, nanoseconds PASS");
	return 0;
}
