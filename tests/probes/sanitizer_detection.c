#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 仅检测器自检：健康、ASan 越界、UBSan 溢出必须得出不同结果。
 * 不链接 Logger、不安装；不能把错误样本的非零退出当作运行失败后忽略。
 */
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "healthy")) {
		char *p = malloc(16);

		if (!p)
			return 3;
		memset(p, 0, 16);
		free(p);
		puts("SANITIZER_HEALTHY=1");
		return 0;
	}
	if (!strcmp(argv[1], "heap-overflow")) {
		volatile size_t size = 16;
		volatile char *p = malloc(size);

		if (!p)
			return 3;
		/* 刻意越界；必须产生 heap-buffer-overflow，而非普通退出码。 */
		p[size] = 1;
		free((void *)p);
	} else if (!strcmp(argv[1], "signed-overflow")) {
		volatile int value = INT_MAX;
		/* 刻意溢出；编译此自检时禁止 UBSan 恢复执行。 */
		volatile int result = value + 1;
		(void)result;
	} else {
		return 2;
	}
	puts("SANITIZER_MISSED_ERROR=1");
	return 0;
}
