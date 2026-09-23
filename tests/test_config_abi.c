#include "audit.h"
#include "console.h"
#include "logger.h"
#include <errno.h>
int main(void)
{
	logger_config_t l = LOGGER_DEFAULT_CONFIG();
	if (l.version != LOGGER_CONFIG_VERSION || l.struct_size != sizeof(l))
		return 1;
	l.version = 999;
	errno = 0;
	if (logger_create(&l) != NULL || errno != EPROTONOSUPPORT)
		return 2;
	l = LOGGER_DEFAULT_CONFIG();
	l.struct_size = 8;
	errno = 0;
	if (logger_create(&l) != NULL || errno != EINVAL)
		return 3;
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	if (a.version != AUDIT_CONFIG_VERSION || a.struct_size != sizeof(a))
		return 4;
	a.version = 999;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EPROTONOSUPPORT)
		return 5;
	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.version != CONSOLE_CONFIG_VERSION || c.struct_size != sizeof(c))
		return 6;
	return 0;
}