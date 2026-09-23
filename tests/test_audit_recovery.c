#include "audit.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void)
{
	unlink("./recover.audit.log");
	unlink("./recover.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "recover";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&c))
		return 1;
	audit_shutdown();
	int fd = open("./recover.audit.log", O_WRONLY | O_APPEND);
	if (fd < 0)
		return 2;
	const char junk[] = "partial-crash-record";
	if (write(fd, junk, sizeof(junk) - 1) != (ssize_t)(sizeof(junk) - 1)) {
		close(fd);
		return 3;
	}
	fsync(fd);
	close(fd);
	struct stat before;
	if (stat("./recover.audit.log", &before))
		return 4;
	if (audit_init(&c))
		return 5;
	audit_shutdown();
	/* The partial tail must be gone; START/STOP may make the repaired file larger. */
	FILE *f = fopen("./recover.audit.log", "r");
	if (!f)
		return 6;
	char buf[65536];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (strstr(buf, "partial-crash-record"))
		return 7;
	if (audit_verify_file("./recover.audit.log"))
		return 8;
	return 0;
}
