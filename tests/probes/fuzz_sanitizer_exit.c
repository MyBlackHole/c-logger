#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 仅回放两个固定输入，不链接 Logger，也不进行随机模糊测试。
 * 必须保留与 public harness 相同的可恢复 UBSan 编译方式，
 * 由两者共同继承的运行时设置决定是否立即失败。
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size != 1)
		return 0;
	if (data[0] == 0) {
		fputs("FUZZ_SANITIZER_HEALTHY=1\n", stderr);
		return 0;
	}
	if (data[0] != 1)
		return 0;
	volatile int value = INT_MAX;
	/* 刻意触发真实 signed integer overflow；恢复执行必须使自检失败。 */
	volatile int result = value + 1;
	(void)result;
	fputs("FUZZ_SANITIZER_MISSED_ERROR=1\n", stderr);
	return 0;
}
