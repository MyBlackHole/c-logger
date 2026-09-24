#include "audit.h"
#include "console.h"
#include "logger.h"
#include <errno.h>

static int rejects_invalid(logger_config_t *c)
{
	errno = 0;
	logger_t *l = logger_create(c);
	if (l) {
		logger_destroy(l);
		return 0;
	}
	return errno == EINVAL;
}

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

	l = LOGGER_DEFAULT_CONFIG();
	l.outputs = 1u << 31;
	if (!rejects_invalid(&l))
		return 4;
	l = LOGGER_DEFAULT_CONFIG();
	l.detail = (logger_detail_t)999;
	if (!rejects_invalid(&l))
		return 5;
	l = LOGGER_DEFAULT_CONFIG();
	l.rotation.mode = (logger_rotate_mode_t)999;
	if (!rejects_invalid(&l))
		return 6;
	l = LOGGER_DEFAULT_CONFIG();
	l.file_mode = 01000;
	if (!rejects_invalid(&l))
		return 7;
	l = LOGGER_DEFAULT_CONFIG();
	l.flush_level = (logger_level_t)999;
	if (!rejects_invalid(&l))
		return 8;
	l = LOGGER_DEFAULT_CONFIG();
	l.overflow[LOGGER_INFO] = (logger_overflow_policy_t)999;
	if (!rejects_invalid(&l))
		return 9;

	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	if (a.version != AUDIT_CONFIG_VERSION || a.struct_size != sizeof(a))
		return 10;
	a.version = 999;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EPROTONOSUPPORT)
		return 11;
	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.version != CONSOLE_CONFIG_VERSION || c.struct_size != sizeof(c))
		return 12;
	return 0;
}
