#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "regression/support.h"

static size_t mask_model(const char *value, size_t pre, size_t suf, char *out)
{
	if (!value) {
		memcpy(out, "[REDACTED]", sizeof("[REDACTED]"));
		return sizeof("[REDACTED]");
	}
	size_t n = strlen(value);

	if (pre >= n) {
		pre = 0;
		suf = 0;
	} else if (suf >= n - pre) {
		pre = 0;
		suf = 0;
	}
	for (size_t i = 0; i < pre; ++i)
		out[i] = value[i];
	for (size_t i = 0; i < 3; ++i)
		out[pre + i] = '*';
	for (size_t i = 0; i < suf; ++i)
		out[pre + 3 + i] = value[n - suf + i];
	out[pre + 3 + suf] = 0;
	return pre + 3 + suf + 1;
}

static void mask_buffer_case(const char *input, size_t pre, size_t suf,
			     size_t cap, size_t offset)
{
	unsigned char arena[128];
	unsigned char before[sizeof(arena)];
	char expected[16];
	size_t need = mask_model(input, pre, suf, expected);
	int expected_rc = !cap ? -EINVAL : (cap < need ? -ENOSPC : 0);

	memset(arena, 0xa5, sizeof(arena));
	memcpy(arena + 64, input, strlen(input) + 1);
	memcpy(before, arena, sizeof(arena));
	errno = EDOM;
	int rc = logger_mask_secret((const char *)arena + 64,
				    (char *)arena + offset, cap, pre, suf);

	CHECK(rc == expected_rc && errno == EDOM);
	if (!rc)
		memcpy(before + offset, expected, need);
	/* 整对象比较同时拒绝失败写槽、源串破坏和成功写出结果范围之外。 */
	CHECK(!memcmp(arena, before, sizeof(arena)));
}

static unsigned int mask_capacities(const char *input, size_t pre, size_t suf)
{
	const size_t offsets[] = { 16, 56, 60, 63, 64, 65, 68, 72, 96 };
	size_t count = sizeof(offsets) / sizeof(offsets[0]);
	unsigned int cases = 0;

	for (size_t cap = 0; cap <= 16; ++cap) {
		for (size_t k = 0; k < count; ++k) {
			mask_buffer_case(input, pre, suf, cap, offsets[k]);
			++cases;
		}
	}
	return cases;
}

static unsigned int mask_contract(void)
{
	unsigned int cases = 0;

	for (size_t n = 0; n <= 8; ++n) {
		char input[9];

		for (size_t i = 0; i < n; ++i)
			input[i] = (char)('a' + i);
		input[n] = 0;
		for (size_t a = 0; a < 12; ++a) {
			for (size_t b = 0; b < 12; ++b) {
				size_t pre = a == 11 ? SIZE_MAX : a;
				size_t suf = b == 11 ? SIZE_MAX : b;

				cases += mask_capacities(input, pre, suf);
			}
		}
	}
	for (size_t cap = 0; cap <= 12; ++cap) {
		unsigned char out[16];
		unsigned char before[sizeof(out)];
		int expected = !cap ? -EINVAL : -ENOSPC;

		if (cap >= sizeof("[REDACTED]"))
			expected = 0;
		memset(out, 0xa5, sizeof(out));
		memcpy(before, out, sizeof(out));
		errno = EDOM;
		CHECK(logger_mask_secret("x", NULL, cap, 0, 0) == -EINVAL);
		CHECK(errno == EDOM);
		int rc = logger_mask_secret(NULL, (char *)out, cap, SIZE_MAX,
					    SIZE_MAX);

		CHECK(rc == expected && errno == EDOM);
		if (!rc)
			memcpy(before, "[REDACTED]", sizeof("[REDACTED]"));
		CHECK(!memcmp(out, before, sizeof(out)));
		cases += 2;
	}
	return cases;
}

int main(void)
{
	unsigned int cases = mask_contract();
	const char *secret = "super-secret-token-12345";
	char b[64];
	if (strcmp(logger_redact(), "[REDACTED]"))
		return 1;
	if (logger_mask_secret(secret, b, sizeof(b), 4, 4))
		return 2;
	if (strstr(b, secret))
		return 3;
	if (strcmp(b, "supe***2345"))
		return 4;
	if (logger_mask_secret("abc", b, sizeof(b), 3, 0))
		return 5;
	if (strcmp(b, "***"))
		return 6; /* 辅助函数拒绝完整披露 */
	errno = EDOM;
	char tiny[3];
	if (logger_mask_secret(secret, tiny, sizeof(tiny), 0, 0) != -ENOSPC ||
	    errno != EDOM)
		return 7;

	/* 上面的纯辅助函数不需建域；真正创建实例前由宿主显式初始化。 */
	if (logger_process_init())
		return 13;
	unlink("./redact.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./redact.log";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.async_mode = 0;
	logger_t *logger = NULL;
	(void)logger_create(&c, &logger);
	if (!logger)
		return 8;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "auth", NULL, 0,
					NULL, "credential=%s", logger_redact());
	int close_rc = logger_destroy(&logger);
	if (rc || close_rc)
		return 9;
	FILE *f = fopen("./redact.log", "r");
	if (!f)
		return 10;
	char data[32768];
	size_t n = fread(data, 1, sizeof(data) - 1, f);
	fclose(f);
	data[n] = 0;
	if (!strstr(data, "[REDACTED]"))
		return 11;
	if (strstr(data, secret))
		return 12;
	printf("MASK_PROPERTY_CASES=%u\n", cases);
	return 0;
}
