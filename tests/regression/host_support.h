#ifndef HOST_TEST_SUPPORT_H
#define HOST_TEST_SUPPORT_H
#include "support.h"
/* Runtime log lines are bounded by LOGGER_LINE_MAX (currently 5120). Search
 * ALL lines, not the first 32 KiB of a concurrent/drain test's output file. */
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
