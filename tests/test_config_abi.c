#include "console.h"
#include "logger.h"
#include <errno.h>
#include <stdint.h>

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

	/* Future callers may append zero fields, but unknown nonzero options
	 * must fail closed. Exercise this through the production Logger API. */
	struct future_logger_config {
		logger_config_t config;
		uint64_t tail;
	} future = {
		.config = LOGGER_DEFAULT_CONFIG(),
		.tail = 0,
	};
	future.config.struct_size = sizeof(future);
	future.config.outputs = LOGGER_OUT_STDERR;
	future.config.async_mode = 0;
	logger_t *instance = logger_create(&future.config);
	if (!instance)
		return 10;
	if (logger_destroy_status(instance))
		return 11;
	future.tail = UINT64_C(1);
	errno = 0;
	instance = logger_create(&future.config);
	if (instance) {
		logger_destroy(instance);
		return 12;
	}
	if (errno != E2BIG)
		return 13;

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
