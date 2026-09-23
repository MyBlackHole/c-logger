#define _GNU_SOURCE
#include "logger_v1.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
	char dir[] = "/tmp/lg-v1-dso-XXXXXX";
	if (!mkdtemp(dir) || chdir(dir))
		return 1;
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "out.log";
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	cfg.queue_capacity = 16;
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	char *base = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED || mprotect(base + page, page, PROT_NONE))
		return 2;
	void *old = base + page - sizeof(cfg);
	memcpy(old, &cfg, sizeof(cfg));
	logger_t *l = logger_create(old);
	if (!l)
		return 3;
	logger_log(l, LOGGER_INFO, "old-dso", NULL, 0, NULL,
		   "old-header-shared-consumer");
	if (logger_flush_instance_status(l) || logger_destroy_status(l))
		return 4;
	FILE *f = fopen("out.log", "r");
	if (!f)
		return 5;
	char b[8192];
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	b[n] = 0;
	fclose(f);
	if (!strstr(b, "old-header-shared-consumer"))
		return 6;
	if (munmap(base, page * 2) || unlink("out.log") ||
	    unlink("out.log.logger.lock") || chdir("/") || rmdir(dir))
		return 7;
	puts("frozen old-header consumer linked to actual shared logger passed");
	return 0;
}
