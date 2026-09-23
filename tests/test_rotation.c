#define LOG_MODULE "rotation"
#include "logger.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	unlink("./rotation.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./rotation.log";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_SIZE;
	c.rotation.max_file_size = 1024;
	c.rotation.retention_days = 1;
	if (logger_init(&c)) {
		perror("logger_init");
		return 1;
	}
	for (int i = 0; i < 100; i++)
		LOG_INFO(
			"record=%d abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
			i);
	logger_shutdown();
	DIR *d = opendir(".");
	if (!d)
		return 2;
	int found = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strncmp(e->d_name, "rotation.", 9) == 0 &&
		    strstr(e->d_name, "Z.log"))
			found = 1;
	closedir(d);
	return found ? 0 : 3;
}
