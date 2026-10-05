#include "audit.h"
#include "console.h"
#include "logger.h"
#include <errno.h>
#include <unistd.h>

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

	/* A pre-1.0 caller may still advertise the historical 72-byte config.
	 * The current library consumes only the known prefix and ignores the tail. */
	a = AUDIT_DEFAULT_CONFIG();
	a.struct_size = sizeof(a) + 8u;
	a.log_dir = ".";
	a.name = "config_abi_tail";
	a.rotation.mode = LOGGER_ROTATE_NONE;
	a.integrity = AUDIT_INTEGRITY_NONE;
	unlink("./config_abi_tail.audit.log");
	unlink("./config_abi_tail.audit.log.logger.lock");
	if (audit_init(&a))
		return 12;
	if (audit_shutdown_status())
		return 13;
	unlink("./config_abi_tail.audit.log");
	unlink("./config_abi_tail.audit.log.logger.lock");

	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.version != CONSOLE_CONFIG_VERSION || c.struct_size != sizeof(c))
		return 14;
	return 0;
}
