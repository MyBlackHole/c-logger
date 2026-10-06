#include "audit.h"
#include "console.h"
#include "logger.h"
#include <errno.h>
#include <stdint.h>
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
	_Static_assert(LOGGER_CONFIG_SIZE == sizeof(logger_config_t),
		       "Logger v1 config size changed");
	_Static_assert(AUDIT_CONFIG_SIZE == sizeof(audit_config_t),
		       "Audit v1 config size changed");
	_Static_assert(CONSOLE_CONFIG_SIZE == sizeof(console_config_t),
		       "Console v1 config size changed");

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

	a = AUDIT_DEFAULT_CONFIG();
	a.struct_size = AUDIT_CONFIG_SIZE - 1u;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EINVAL)
		return 12;

	struct future_audit_config {
		audit_config_t config;
		uint64_t tail;
	};
	_Static_assert(sizeof(struct future_audit_config) == 72,
		       "future Audit fixture extent changed");

	struct future_audit_config future = {
		.config = AUDIT_DEFAULT_CONFIG(),
		.tail = 0,
	};
	future.config.struct_size = sizeof(future);
	future.config.log_dir = ".";
	future.config.name = "config_abi_future";
	future.config.rotation.mode = LOGGER_ROTATE_NONE;
	future.config.integrity = AUDIT_INTEGRITY_NONE;
	unlink("./config_abi_future.audit.log");
	unlink("./config_abi_future.audit.log.logger.lock");
	if (audit_init(&future.config))
		return 13;
	if (audit_shutdown_status())
		return 14;
	unlink("./config_abi_future.audit.log");
	unlink("./config_abi_future.audit.log.logger.lock");

	future.config = AUDIT_DEFAULT_CONFIG();
	future.config.struct_size = sizeof(future);
	future.tail = UINT64_C(1);
	errno = 0;
	if (audit_init(&future.config) == 0 || errno != E2BIG)
		return 15;

	console_config_t con = CONSOLE_DEFAULT_CONFIG();
	if (con.version != CONSOLE_CONFIG_VERSION ||
	    con.struct_size != sizeof(con))
		return 16;

	struct future_console_config {
		console_config_t config;
		uint64_t tail;
	} future_console = {
		.config = CONSOLE_DEFAULT_CONFIG(),
		.tail = 0,
	};
	future_console.config.struct_size = sizeof(future_console);
	errno = 0;
	console_init(&future_console.config);
	if (errno)
		return 17;

	future_console.config = CONSOLE_DEFAULT_CONFIG();
	future_console.config.struct_size = sizeof(future_console);
	future_console.tail = UINT64_C(1);
	errno = 0;
	console_init(&future_console.config);
	if (errno != E2BIG)
		return 18;

	return 0;
}
