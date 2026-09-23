#include "audit.h"
#include "logger.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int strict_logger(void)
{
	unlink("./fault.log");
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.file_path = "./fault.log";
	c.async_mode = 0;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&c);
	if (!l)
		return 1;
	int rc = logger_log_sync_status(l, LOGGER_INFO, "T", __FILE__, __LINE__,
					__func__, "fault probe");
	logger_destroy(l);
	return rc < 0 ? 0 : 2;
}
static int audit_case(void)
{
	unlink("./faultaudit.audit.log");
	unlink("./faultaudit.audit.state");
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	c.log_dir = ".";
	c.name = "faultaudit";
	c.rotation.mode = LOGGER_ROTATE_NONE;
	int rc = audit_init(&c);
	if (rc == 0)
		audit_shutdown();
	return rc < 0 ? 0 : 3;
}
int main(int argc, char **argv)
{
	if (argc != 2)
		return 99;
	if (!strcmp(argv[1], "logger"))
		return strict_logger();
	if (!strcmp(argv[1], "audit"))
		return audit_case();
	return 98;
}
