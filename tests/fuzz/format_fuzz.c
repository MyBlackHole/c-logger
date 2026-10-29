#define _POSIX_C_SOURCE 200809L
#include "logger_internal.h"
#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 私有格式化入口来自生产静态库，不构造假的 logger_t。
 * 预期文本不调用库内格式化、basename、level_name 或截断辅助函数。
 */
#define LIMIT 8192u
#define GUARD 16u
#ifdef LOGGER_FUZZ_STANDALONE
static unsigned int negative_checks;
#endif
static int exercise_oracle;

static void fail(const char *condition, int line)
{
	fprintf(stderr, "format:%d: %s errno=%d\n", line, condition, errno);
	abort();
}
#define REQUIRE(c) ((c) ? (void)0 : fail(#c, __LINE__))

struct sample {
	struct logger_message message;
	logger_config_t config;
	char module[513], path[520], function[257];
	char expected[LIMIT];
	size_t length;
	int invalid_nsec;
};

static unsigned int byte(const uint8_t *data, size_t size, size_t offset)
{
	return offset < size ? data[offset] : 0;
}

static unsigned int word(const uint8_t *data, size_t size, size_t offset)
{
	return byte(data, size, offset) | (byte(data, size, offset + 1) << 8);
}

static void text(char *out, size_t length, const uint8_t *data, size_t size)
{
	for (size_t i = 0; i < length; ++i)
		out[i] = (char)('a' + byte(data, size, 16 + i % 32) % 26);
	out[length] = 0;
}

static void append(struct sample *s, const char *part)
{
	size_t length = strlen(part);
	REQUIRE(length < sizeof(s->expected) - s->length);
	memcpy(s->expected + s->length, part, length + 1);
	s->length += length;
}

static void number(struct sample *s, long value)
{
	char part[32];
	int n = snprintf(part, sizeof(part), "%ld", value);
	REQUIRE(n > 0 && (size_t)n < sizeof(part));
	append(s, part);
}

static const char *reference_time(unsigned int index, time_t *sec)
{
	switch (index) {
	case 0:
		*sec = 1700000000;
		return "2023-11-14T22:13:20";
	case 1:
		*sec = 951782400;
		return "2000-02-29T00:00:00";
	default:
		*sec = 2147483647;
		return "2038-01-19T03:14:07";
	}
}

static const char *reference_level(unsigned int level)
{
	switch (level) {
	case 0:
		return "TRACE";
	case 1:
		return "DEBUG";
	case 2:
		return "INFO";
	case 3:
		return "WARN";
	case 4:
		return "ERROR";
	case 5:
		return "FATAL";
	case 6:
		return "OFF";
	default:
		return "UNKNOWN";
	}
}

static void prepare(struct sample *s, const uint8_t *data, size_t size)
{
	unsigned int options = byte(data, size, 0);
	unsigned int level = byte(data, size, 1) % 8u;
	unsigned int time_index = byte(data, size, 2) % 3u;
	struct logger_message *m = &s->message;
	logger_config_t *c = &s->config;
	*c = LOGGER_DEFAULT_CONFIG();
	c->outputs = LOGGER_OUT_STDERR;
	c->async_mode = 0;
	c->detail = (logger_detail_t)(options & 3u);
	c->include_pid = (options >> 2) & 1u;
	c->include_tid = (options >> 3) & 1u;
	c->include_source = (options >> 4) & 1u;
	m->level = level == 7 ? (logger_level_t)255 : (logger_level_t)level;
	const char *date = reference_time(time_index, &m->ts.tv_sec);
	m->ts.tv_nsec = (long)word(data, size, 5) * 15001L;
	m->pid = (pid_t)word(data, size, 9);
	m->tid = m->pid + 17;
	m->line = (int)word(data, size, 12);
	text(m->text, word(data, size, 7) % LOGGER_MESSAGE_MAX, data, size);
	text(s->module, byte(data, size, 9) * 2u, data, size);
	memcpy(s->path, "parent/", 7);
	text(s->path + 7, byte(data, size, 10) * 2u, data, size);
	text(s->function, byte(data, size, 11), data, size);
	text(m->request_id, byte(data, size, 12) % 64u, data, size);
	text(m->session_id, byte(data, size, 13) % 64u, data, size);
	text(m->trace_id, byte(data, size, 14) % 64u, data, size);
	unsigned int nulls = byte(data, size, 15);
	m->module = (nulls & 1u) ? NULL : s->module;
	m->file = (nulls & 2u) ? NULL : s->path;
	m->func = (nulls & 4u) ? NULL : s->function;

	append(s, date);
	char fractional[32];
	int n = snprintf(fractional, sizeof(fractional), ".%06ld+0000 ",
			 m->ts.tv_nsec / 1000L);
	REQUIRE(n > 0 && (size_t)n < sizeof(fractional));
	append(s, fractional);
	const char *level_name = reference_level(level);
	append(s, level_name);
	for (size_t i = strlen(level_name); i < 5; ++i)
		append(s, " ");
	if (c->detail >= LOGGER_DETAIL_VERBOSE && c->include_pid) {
		append(s, " pid=");
		number(s, m->pid);
	}
	if (c->detail >= LOGGER_DETAIL_VERBOSE && c->include_tid) {
		append(s, " tid=");
		number(s, m->tid);
	}
	if (c->detail >= LOGGER_DETAIL_NORMAL) {
		append(s, " [");
		append(s, m->module ? m->module : "app");
		append(s, "]");
	}
	if (c->detail >= LOGGER_DETAIL_DEBUG) {
		if (m->request_id[0]) {
			append(s, " request=");
			append(s, m->request_id);
		}
		if (m->session_id[0]) {
			append(s, " session=");
			append(s, m->session_id);
		}
		if (m->trace_id[0]) {
			append(s, " trace=");
			append(s, m->trace_id);
		}
		if (c->include_source) {
			append(s, " ");
			append(s, m->file ? s->path + 7 : "?");
			append(s, ":");
			number(s, m->line);
			append(s, " ");
			append(s, m->func ? m->func : "?");
			append(s, "()");
		}
	}
	append(s, " ");
	append(s, m->text);
	append(s, "\n");
	s->invalid_nsec = byte(data, size, 3) % 5u < 2u;
}

/* 校验器对结果码、长度、有效字节及两侧边界都独立判定。 */
static int matches(const unsigned char *buffer, size_t cap, int rc, size_t n,
		   int wanted_rc, const char *wanted, size_t wanted_n)
{
	for (size_t i = 0; i < GUARD; ++i)
		if (buffer[i] != 0xa5 || buffer[GUARD + cap + i] != 0xa5)
			return 0;
	if (rc != wanted_rc || n != wanted_n)
		return 0;
	if (!cap)
		return n == 0;
	if (n >= cap)
		return 0;
	return !memcmp(buffer + GUARD, wanted, n + 1);
}

static void negative_cases(unsigned char *buffer, size_t cap, size_t n,
			   const char *wanted)
{
	REQUIRE(!matches(buffer, cap, -EINVAL, n, 0, wanted, n));
	REQUIRE(!matches(buffer, cap, 0, n - 1, 0, wanted, n));
	buffer[GUARD + n - 1] ^= 1;
	REQUIRE(!matches(buffer, cap, 0, n, 0, wanted, n));
	buffer[GUARD + n - 1] ^= 1;
	buffer[GUARD + n] = 'x';
	REQUIRE(!matches(buffer, cap, 0, n, 0, wanted, n));
	buffer[GUARD + n] = 0;
	buffer[0] = 0;
	REQUIRE(!matches(buffer, cap, 0, n, 0, wanted, n));
	buffer[0] = 0xa5;
	buffer[GUARD + cap] = 0;
	REQUIRE(!matches(buffer, cap, 0, n, 0, wanted, n));
	buffer[GUARD + cap] = 0xa5;
#ifdef LOGGER_FUZZ_STANDALONE
	negative_checks += 6;
#endif
}

static void check_capacity(logger_t *logger, struct sample *s, size_t cap,
			   int recovery)
{
	REQUIRE(cap <= LIMIT);
	unsigned char *buffer = malloc(cap + 2 * GUARD);
	REQUIRE(buffer);
	memset(buffer, 0xa5, cap + 2 * GUARD);
	char *out = (char *)buffer + GUARD;
	size_t n = SIZE_MAX;
	int rc = logger_format_line_checked(logger, &s->message, out, cap, &n);
	int expected_rc = 0;
	if (cap < 2)
		expected_rc = -EOVERFLOW;
	else if (s->invalid_nsec)
		expected_rc = -EINVAL;
	else if (cap <= s->length)
		expected_rc = -EOVERFLOW;
	const char *want = expected_rc ? "" : s->expected;
	size_t want_n = expected_rc ? 0 : s->length;
	int ok = matches(buffer, cap, rc, n, expected_rc, want, want_n);
	REQUIRE(ok);
	if (!s->invalid_nsec) {
		char wanted[LIMIT];
		size_t length = 0;
		wanted[0] = 0;
		if (cap > s->length) {
			length = s->length;
			memcpy(wanted, s->expected, length + 1);
		} else if (cap >= 2) {
			length = cap - 1;
			memcpy(wanted, s->expected, cap - 2);
			const char *mark = " [truncated]";
			size_t size = strlen(mark);
			if (cap >= size + 2)
				memcpy(wanted + cap - size - 2, mark, size);
			else if (cap >= 3)
				wanted[cap - 3] = '~';
			wanted[cap - 2] = '\n';
			wanted[cap - 1] = 0;
		}
		memset(buffer, 0xa5, cap + 2 * GUARD);
		n = logger_format_line(logger, &s->message, out, cap);
		REQUIRE(matches(buffer, cap, 0, n, 0, wanted, length));
		if (exercise_oracle && recovery && cap > s->length)
			negative_cases(buffer, cap, n, wanted);
	}
	free(buffer);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size > 512)
		return 0;
	struct sample *s = calloc(1, sizeof(*s));
	REQUIRE(s);
	prepare(s, data, size);
	logger_t *logger = NULL;
	REQUIRE(logger_create(&s->config, &logger) == 0);
	REQUIRE(logger != NULL);
	long valid_nsec = s->message.ts.tv_nsec;
	if (s->invalid_nsec) {
		s->message.ts.tv_nsec = -1;
		if (byte(data, size, 3) & 1u)
			s->message.ts.tv_nsec = 1000000000L;
	}
	size_t choice = byte(data, size, 4) % 8u;
	size_t cap = LOGGER_LINE_MAX;
	if (choice < 3)
		cap = choice;
	else if (choice < 6)
		cap = s->length + choice - 3;
	else if (choice == 6)
		cap = word(data, size, 5) % LIMIT;
	check_capacity(logger, s, cap, 0);
	/* 每次失败后恢复纳秒，重测完整输出，覆盖复用相同时间戳缓存。 */
	s->message.ts.tv_nsec = valid_nsec;
	s->invalid_nsec = 0;
	check_capacity(logger, s, s->length + 1, 1);
	REQUIRE(!logger_destroy(&logger));
	free(s);
	return 0;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	REQUIRE(logger_process_init() == 0);
	REQUIRE(!setenv("TZ", "UTC0", 1));
	REQUIRE(setlocale(LC_TIME, "C"));
	tzset();
	return 0;
}

#ifdef LOGGER_FUZZ_STANDALONE
int main(int argc, char **argv)
{
	REQUIRE(argc == 2);
	REQUIRE(!LLVMFuzzerInitialize(&argc, &argv));
	exercise_oracle = !strcmp(argv[1], "selftest");
	char *end = NULL;
	errno = 0;
	unsigned long seed = exercise_oracle ? 107 : strtoul(argv[1], &end, 10);
	if (!exercise_oracle) {
		REQUIRE(end != argv[1] && *end == 0);
		REQUIRE(!errno && seed <= UINT32_MAX);
	}
	uint32_t state = seed ? (uint32_t)seed : UINT32_C(0x9e3779b9);
	for (unsigned int i = 0; i < 512; ++i) {
		uint8_t input[64];
		for (size_t j = 0; j < sizeof(input); ++j) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			input[j] = (uint8_t)state;
		}
		input[0] = (input[0] & ~3u) | ((i / 8u) & 3u);
		input[4] = (uint8_t)(i % 8u);
		REQUIRE(!LLVMFuzzerTestOneInput(input, sizeof(input)));
	}
	REQUIRE(!exercise_oracle || negative_checks == 512u * 6u);
	printf("format PASS seed=%lu inputs=512 rejected=%u\n", seed,
	       negative_checks);
	return 0;
}
#endif
