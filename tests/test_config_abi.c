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

	_Static_assert(AUDIT_CONFIG_V1_PREFIX_SIZE <= sizeof(audit_config_t),
		       "Audit v1 prefix exceeds current config");
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	if (a.version != AUDIT_CONFIG_VERSION || a.struct_size != sizeof(a))
		return 10;
	a.version = 999;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EPROTONOSUPPORT)
		return 11;

	/* The pre-v1 zero/zero escape hatch must not enter SONAME 1. */
	a = AUDIT_DEFAULT_CONFIG();
	a.struct_size = 0;
	a.version = 0;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EINVAL)
		return 12;

	/* A versioned caller must provide the complete stable v1 prefix. */
	a = AUDIT_DEFAULT_CONFIG();
	a.struct_size = AUDIT_CONFIG_V1_PREFIX_SIZE - 1u;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EINVAL)
		return 13;

	/* A pre-1.0 caller may still provide the historical 72-byte config.
	 * Model the real memory extent: current 64-byte prefix plus an unknown
	 * 8-byte tail. The library must consume only the known prefix. */
	struct legacy_audit_config {
		audit_config_t prefix;
		uint64_t retired_tail;
	};
	_Static_assert(sizeof(struct legacy_audit_config) == 72,
		       "historical Audit config extent changed");
	struct legacy_audit_config legacy = {
		.prefix = AUDIT_DEFAULT_CONFIG(),
		.retired_tail = UINT64_C(0xa5a5a5a5a5a5a5a5),
	};
	legacy.prefix.struct_size = sizeof(legacy);
	legacy.prefix.log_dir = ".";
	legacy.prefix.name = "config_abi_tail";
	legacy.prefix.rotation.mode = LOGGER_ROTATE_NONE;
	legacy.prefix.integrity = AUDIT_INTEGRITY_NONE;
	unlink("./config_abi_tail.audit.log");
	unlink("./config_abi_tail.audit.log.logger.lock");
	if (audit_init(&legacy.prefix))
		return 14;
	if (audit_shutdown_status())
		return 15;
	if (legacy.retired_tail != UINT64_C(0xa5a5a5a5a5a5a5a5))
		return 16;
	unlink("./config_abi_tail.audit.log");
	unlink("./config_abi_tail.audit.log.logger.lock");

	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.version != CONSOLE_CONFIG_VERSION || c.struct_size != sizeof(c))
		return 17;
	return 0;
}
