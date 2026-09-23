/* Frozen prior headers; public calls only, used by an already-linked executable. */
#include "logger.h"
#include "audit.h"
#include <errno.h>
#include <string.h>
int main(void)
{
	logger_config_t cfg = LOGGER_DEFAULT_CONFIG();
	cfg.outputs = LOGGER_OUT_FILE;
	cfg.file_path = "legacy-consumer.log";
	cfg.async_mode = 0;
	cfg.rotation.mode = LOGGER_ROTATE_NONE;
	logger_t *l = logger_create(&cfg);
	if (!l)
		return 1;
	LOGGER_INFO(l, "legacy", "previous-header consumer");
	if (logger_destroy_status(l))
		return 2;
	audit_config_t a = AUDIT_DEFAULT_CONFIG();
	a.log_dir = ".";
	a.name = "legacy-consumer";
	a.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&a) || audit_shutdown_status())
		return 3;
	if (audit_verify_file("legacy-consumer.audit.log"))
		return 4;
	a.integrity = (audit_integrity_t)2;
	if (audit_init(&a) != -1 || errno != EPROTONOSUPPORT)
		return 5;
	return strcmp(audit_crypto_backend(), "builtin") ? 6 : 0;
}
