#include "logger.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 同一入口可由 clang -fsanitize=fuzzer 或固定种子回放驱动。
 * 只调用已经安装的公开 Logger API，不包含私有故障钩子。
 */
static void fuzz_fail(const char *condition, int line)
{
	fprintf(stderr, "%d: %s errno=%d\n", line, condition, errno);
	abort();
}

#define REQUIRE(cond) ((cond) ? (void)0 : fuzz_fail(#cond, __LINE__))

static void mask_property(const uint8_t *data, size_t size)
{
	if (size < 5)
		return;
	char secret[97];
	size_t n = size - 5;
	if (n > sizeof(secret) - 1)
		n = sizeof(secret) - 1;
	for (size_t i = 0; i < n; ++i)
		secret[i] = (char)('a' + data[5 + i] % 26);
	secret[n] = '\0';

	const char *value = (data[1] & 1u) ? NULL : secret;
	size_t pre = data[2], suf = data[3];
	size_t cap = data[4] % 128u;

	/* 前后两侧的哨兵字节不能被用户提供的 cap 越界写坏。 */
	unsigned char result[144];
	memset(result, 0xa5, sizeof(result));
	char *out = (char *)&result[8];
	errno = 0;
	int rc = logger_mask_secret(value, out, cap, pre, suf);
	size_t expected_length;
	char expected[128];
	if (!value) {
		memcpy(expected, "[REDACTED]", sizeof("[REDACTED]"));
		expected_length = sizeof("[REDACTED]");
	} else {
		if (pre > n)
			pre = n;
		if (suf > n - pre)
			suf = n - pre;
		/* 任何恰好暴露整个秘密的参数必须退化为全遮挡。 */
		if (pre + suf >= n) {
			pre = 0;
			suf = 0;
		}
		memcpy(expected, secret, pre);
		memcpy(expected + pre, "***", 3);
		memcpy(expected + pre + 3, secret + n - suf, suf);
		expected_length = pre + 3 + suf + 1;
		expected[expected_length - 1] = '\0';
	}
	if (!cap) {
		REQUIRE(rc == -1 && errno == EINVAL);
	} else if (cap < expected_length) {
		REQUIRE(rc == -1 && errno == ENOSPC);
	} else {
		REQUIRE(rc == 0);
		REQUIRE(!memcmp(out, expected, expected_length));
		REQUIRE(strlen(out) == expected_length - 1);
	}
	for (size_t i = 0; i < 8; ++i)
		REQUIRE(result[i] == 0xa5);
	for (size_t i = 8 + cap; i < sizeof(result); ++i)
		REQUIRE(result[i] == 0xa5);
}

static void config_property(const uint8_t *data, size_t size)
{
	if (size < 3)
		return;
	struct {
		logger_config_t config;
		unsigned char tail[16];
	} future = { 0 };
	future.config = LOGGER_DEFAULT_CONFIG();
	future.config.outputs = LOGGER_OUT_STDERR;
	future.config.async_mode = 0;
	int expected_error = 0;

	switch (data[1] % 13u) {
	case 0:
		/* 合法版本，显式实例创建并被同一个所有者销毁。 */
		break;
	case 1:
		future.config.version = UINT32_MAX;
		expected_error = EPROTONOSUPPORT;
		break;
	case 2:
		--future.config.struct_size;
		expected_error = EINVAL;
		break;
	case 3:
		future.config.struct_size = sizeof(future);
		break;
	case 4:
		future.config.struct_size = sizeof(future);
		future.tail[data[2] % sizeof(future.tail)] = 1;
		expected_error = E2BIG;
		break;
	case 5:
		future.config.outputs = 0;
		expected_error = EINVAL;
		break;
	case 6:
		future.config.detail = (logger_detail_t)999;
		expected_error = EINVAL;
		break;
	case 7:
		future.config.rotation.mode = (logger_rotate_mode_t)999;
		expected_error = EINVAL;
		break;
	case 8:
		future.config.overflow[LOGGER_INFO] =
			(logger_overflow_policy_t)999;
		expected_error = EINVAL;
		break;
	case 9:
		future.config.file_mode = 01000;
		expected_error = EINVAL;
		break;
	case 10:
		future.config.flush_level = (logger_level_t)999;
		expected_error = EINVAL;
		break;
	case 11:
		future.config.level = (logger_level_t)999;
		expected_error = EINVAL;
		break;
	default:
		future.config.outputs = 1u << 31;
		expected_error = EINVAL;
		break;
	}

	errno = 0;
	logger_t *instance = NULL;
	int create_rc = logger_create(&future.config, &instance);
	if (expected_error) {
		REQUIRE(instance == NULL);
		REQUIRE(create_rc == -expected_error);
	} else {
		REQUIRE(instance != NULL && create_rc == 0);
		REQUIRE(logger_destroy(&instance) == 0);
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	REQUIRE(logger_process_init() == 0);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* 模糊输入只选择受界配置与长度，永不转为悬空外部指针、
	 * 无界工作线程、任意文件路径或未经验证的 struct_size。
	 */
	if (!data || size < 3 || size > 512)
		return 0;
	if (data[0] & 1u)
		mask_property(data, size);
	else
		config_property(data, size);
	return 0;
}

#ifdef LOGGER_FUZZ_STANDALONE
static uint32_t next_random(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

int main(int argc, char **argv)
{
	REQUIRE(argc == 2);
	REQUIRE(LLVMFuzzerInitialize(&argc, &argv) == 0);
	char *end = NULL;
	unsigned long seed = strtoul(argv[1], &end, 10);
	REQUIRE(end && end != argv[1] && *end == '\0');
	uint32_t state = (uint32_t)seed;
	if (!state)
		state = UINT32_C(0x9e3779b9);
	for (unsigned i = 0; i < 2048; ++i) {
		uint8_t bytes[128];
		size_t length = 5 + next_random(&state) % 120u;
		for (size_t j = 0; j < length; ++j)
			bytes[j] = (uint8_t)next_random(&state);
		bytes[0] = (uint8_t)((bytes[0] & ~1u) | (i & 1u));
		if (!(i & 1u))
			bytes[1] = (uint8_t)(i % 13u);
		REQUIRE(LLVMFuzzerTestOneInput(bytes, length) == 0);
	}
	printf("public fuzz properties PASS seed=%lu iterations=2048\n", seed);
	return 0;
}
#endif
