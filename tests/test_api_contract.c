#include "audit.h"
#include "logger.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
	errno = 0;
	if (logger_create(NULL) != NULL || errno != EINVAL)
		return 1;
	unlink("./api.audit.log");
	unlink("./api.audit.state");
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	a.log_dir = ".";
	a.name = "api";
	a.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&a))
		return 2;
	char id[33];
	if (audit_instance_id_copy(id))
		return 3;
	if (strlen(id) != 32)
		return 4;
	audit_shutdown();
	errno = 0;
	if (audit_instance_id_copy(id) == 0 || errno != ENODEV)
		return 5;
	return 0;
}
