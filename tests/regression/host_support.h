#ifndef HOST_TEST_SUPPORT_H
#define HOST_TEST_SUPPORT_H
#include "support.h"
/*
 * 运行时日志行受 LOGGER_LINE_MAX 限制（当前为 5120）。
 * 搜索所有行，而不是并发或排空测试输出文件的前 32 KiB。
 */
static inline int log_contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	char line[16384];
	int found = 0;
	while (fgets(line, sizeof(line), f))
		found |= strstr(line, needle) != NULL;
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return found;
}
#endif
