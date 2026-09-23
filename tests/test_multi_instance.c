#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int contains(const char *p, const char *s)
{
	FILE *f = fopen(p, "r");
	if (!f)
		return 0;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	return strstr(b, s) != 0;
}
int main(void)
{
	unlink("./mi-a.log");
	unlink("./mi-b.log");
	logger_config_t a = LOGGER_DEFAULT_CONFIG();
	a.outputs = LOGGER_OUT_FILE;
	a.file_path = "./mi-a.log";
	a.async_mode = 0;
	a.rotation.mode = LOGGER_ROTATE_NONE;
	logger_config_t b = LOGGER_DEFAULT_CONFIG();
	b.outputs = LOGGER_OUT_FILE;
	b.file_path = "./mi-b.log";
	b.async_mode = 0;
	b.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *A = logger_create(&a), *B = logger_create(&b);
	if (!A || !B)
		return 1;
	LOGGER_INFO(A, "alpha", "only-a=%d", 1);
	LOGGER_ERROR(B, "beta", "only-b=%d", 2);
	logger_destroy(A);
	logger_destroy(B);
	if (!contains("./mi-a.log", "only-a=1") ||
	    contains("./mi-a.log", "only-b=2"))
		return 2;
	if (!contains("./mi-b.log", "only-b=2") ||
	    contains("./mi-b.log", "only-a=1"))
		return 3;
	return 0;
}