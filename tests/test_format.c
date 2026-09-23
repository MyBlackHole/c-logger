#define LOG_MODULE "storage"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int emit_one(void)
{
	LOG_ERROR("write failed errno=%d", 5);
	return 0;
}
int main(void)
{
	unlink("./format.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./format.log";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.flush_level = LOGGER_ERROR;
	c.detail = LOGGER_DETAIL_DEBUG;
	if (logger_init(&c))
		return 1;
	emit_one();
	logger_shutdown();
	FILE *f = fopen("./format.log", "r");
	if (!f)
		return 2;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	b[n] = 0;
	if (!strstr(b, " ERROR "))
		return 3;
	if (!strstr(b, " pid=") || !strstr(b, " tid="))
		return 4;
	if (!strstr(b, " [storage]"))
		return 5;
	if (!strstr(b, " test_format.c:"))
		return 6;
	if (!strstr(b, " emit_one()"))
		return 7;
	if (!strstr(b, "write failed errno=5"))
		return 8;
	return 0;
}
