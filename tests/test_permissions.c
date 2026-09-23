#include "audit.h"
#include "logger.h"
#include <sys/stat.h>
#include <unistd.h>
int main(void)
{
	unlink("./perm.log");
	unlink("./permaudit.audit.log");
	unlink("./permaudit.audit.state");
	unlink("./permaudit.audit.lock");
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	l.outputs = LOGGER_OUT_FILE;
	l.file_path = "./perm.log";
	l.async_mode = 0;
	l.rotation.mode = LOGGER_ROTATE_NONE;
	l.file_mode = 0640;
	logger_t *x = logger_create(&l);
	if (!x)
		return 1;
	logger_destroy(x);
	struct stat st;
	if (stat("./perm.log", &st))
		return 2;
	if ((st.st_mode & 0777) != 0640)
		return 3;
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	a.log_dir = ".";
	a.name = "permaudit";
	a.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&a))
		return 4;
	audit_shutdown();
	if (stat("./permaudit.audit.log", &st))
		return 5;
	if ((st.st_mode & 0777) != 0600)
		return 6;
	return 0;
}
