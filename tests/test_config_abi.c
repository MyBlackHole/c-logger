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

	_Static_assert(AUDIT_CONFIG_V1_SIZE == sizeof(audit_config_t),
		       "Audit v1 config size changed");
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
	a.struct_size = AUDIT_CONFIG_V1_SIZE - 1u;
	errno = 0;
	if (audit_init(&a) == 0 || errno != EINVAL)
		return 13;

	/* Production v1 intentionally rejects oversized pre-v1/future layouts.
	 * A layout change must use a new config version, not an implicit tail. */
	struct oversized_audit_config {
		audit_config_t config;
		uint64_t tail;
	};
	_Static_assert(sizeof(struct oversized_audit_config) == 72,
		       "oversized fixture extent changed");
	struct oversized_audit_config oversized = {
		.config = AUDIT_DEFAULT_CONFIG(),
		.tail = UINT64_C(0xa5a5a5a5a5a5a5a5),
	};
	oversized.config.struct_size = sizeof(oversized);
	errno = 0;
	if (audit_init(&oversized.config) == 0 || errno != EINVAL)
		return 14;
	if (oversized.tail != UINT64_C(0xa5a5a5a5a5a5a5a5))
		return 15;

	console_config_t c = CONSOLE_DEFAULT_CONFIG();
	if (c.version != CONSOLE_CONFIG_VERSION || c.struct_size != sizeof(c))
		return 16;
	return 0;
}
