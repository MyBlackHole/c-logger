#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* 参考结果从调用前的独立输入构造，不能读取被重叠写入后的源串。 */
static size_t expected_mask(const char *value, size_t pre, size_t suf,
			    char *out)
{
	if (!value) {
		memcpy(out, "[REDACTED]", sizeof("[REDACTED]"));
		return sizeof("[REDACTED]");
	}
	size_t n = strlen(value);
	size_t head = pre < n ? pre : n;
	size_t tail = suf < n - head ? suf : n - head;
	if (head + tail == n) {
		head = 0;
		tail = 0;
	}
	size_t used = 0;
	for (size_t i = 0; i < head; ++i)
		out[used++] = value[i];
	for (unsigned i = 0; i < 3; ++i)
		out[used++] = '*';
	for (size_t i = n - tail; i < n; ++i)
		out[used++] = value[i];
	out[used++] = '\0';
	return used;
}

static unsigned long mask_cases;

static void check_mask_case(const char *value, size_t pre, size_t suf,
			    size_t offset, size_t cap)
{
	unsigned char arena[128], expected[128];
	char result[32];
	size_t need = expected_mask(value, pre, suf, result);
	int error = !cap ? -EINVAL : cap < need ? -ENOSPC : 0;

	memset(arena, 0xa5, sizeof(arena));
	if (value)
		memcpy(arena + 48, value, strlen(value) + 1);
	memcpy(expected, arena, sizeof(expected));
	if (!error)
		memcpy(expected + offset, result, need);
	errno = EDOM;
	int rc = logger_mask_secret(value ? (const char *)arena + 48 : NULL,
				    (char *)arena + offset, cap, pre, suf);
	int observed = errno;

	if (rc != error || observed != EDOM ||
	    memcmp(arena, expected, sizeof(arena))) {
		fprintf(stderr,
			"MASK_CASE_FAIL n=%zu pre=%zu suf=%zu offset=%zu "
			"cap=%zu rc=%d expected=%d errno=%d\n",
			value ? strlen(value) : 0, pre, suf, offset, cap,
			rc, error, observed);
		exit(20);
	}
	++mask_cases;
}

static void mask_matrix(void)
{
	/* 先固定原缺陷：遮挡扩大时不能提前破坏尚未读取的后缀。 */
	char alias[32] = "abcdef";
	if (logger_mask_secret(alias, alias, sizeof(alias), 1, 4) ||
	    strcmp(alias, "a***cdef")) {
		fprintf(stderr, "MASK_ALIAS_FAIL actual=%s expected=a***cdef\n",
			alias);
		exit(21);
	}
	for (size_t n = 0; n <= 16; ++n) {
		char value[17];
		for (size_t i = 0; i < n; ++i)
			value[i] = (char)('a' + i);
		value[n] = '\0';
		for (size_t pre = 0; pre <= n + 2; ++pre) {
			for (size_t suf = 0; suf <= n + 2; ++suf) {
				char result[32];
				size_t need = expected_mask(value, pre, suf,
							    result);
				const size_t caps[] = { 0, need - 1, need, 32 };
				for (size_t offset = 16; offset <= 80; ++offset) {
					for (size_t i = 0; i < 4; ++i)
						check_mask_case(value, pre, suf,
								offset, caps[i]);
				}
			}
		}
	}
	/* SIZE_MAX 是真实可传入的前后缀计数；钳制不能先发生加法回绕。 */
	const size_t counts[] = { 0, 1, 4, 6, SIZE_MAX };
	for (size_t i = 0; i < 5; ++i) {
		for (size_t j = 0; j < 5; ++j) {
			for (size_t offset = 16; offset <= 80; ++offset)
				check_mask_case("abcdef", counts[i], counts[j],
						offset, 32);
		}
	}
	for (size_t cap = 0; cap <= 12; ++cap)
		check_mask_case(NULL, SIZE_MAX, SIZE_MAX, 48, cap);
	for (size_t cap = 0; cap <= 32; ++cap) {
		errno = EDOM;
		int rc = logger_mask_secret("abc", NULL, cap, 0, 0);
		if (rc != -EINVAL || errno != EDOM)
			exit(22);
	}
	printf("MASK_MATRIX_PASS cases=%lu\n", mask_cases);
}

int main(void)
{
	mask_matrix();
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
	char tiny[3] = { 'X', 'Y', 'Z' };
	if (logger_mask_secret(secret, tiny, sizeof(tiny), 0, 0) != -ENOSPC ||
	    errno != EDOM || memcmp(tiny, "XYZ", sizeof(tiny)))
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
	int create_rc = logger_create(&c, &logger);
	if (create_rc || !logger)
		return 8;
	int rc = logger_log_sync_status(logger, LOGGER_INFO, "auth", NULL, 0,
					NULL, "credential=%s", logger_redact());
	int close_rc = logger_destroy(&logger);
	if (rc || close_rc)
		return 9;
	FILE *f = fopen("./redact.log", "r");
	if (!f)
		return 10;
	char data[8192];
	size_t n = fread(data, 1, sizeof(data) - 1, f);
	int read_error = ferror(f);
	if (fclose(f) || read_error)
		return 14;
	data[n] = 0;
	if (!strstr(data, "[REDACTED]"))
		return 11;
	if (strstr(data, secret))
		return 12;
	return 0;
}
