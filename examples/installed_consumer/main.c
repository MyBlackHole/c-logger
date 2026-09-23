#include <logger.h>
#include <audit.h>
#include <console.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	logger_config_t c = LOGGER_DEFAULT_CONFIG();
	c.outputs = LOGGER_OUT_FILE;
	c.rotation.mode = LOGGER_ROTATE_NONE;
	c.queue_capacity = 64;
	c.file_path = "installed-a.log";
	logger_t *a = logger_create(&c);
	if (!a) {
		perror("logger_create(a)");
		return 1;
	}
	c.file_path = "installed-b.log";
	c.async_mode = 0;
	logger_t *b = logger_create(&c);
	if (!b) {
		perror("logger_create(b)");
		logger_destroy(a);
		return 2;
	}
	for (unsigned i = 0; i < 16; ++i) {
		LOGGER_INFO(a, "host-a", "record=%u", i);
		LOGGER_INFO(b, "host-b", "record=%u", i);
	}
	int rc = logger_flush_instance_status(a);
	logger_io_metrics_t io;
	logger_get_io_metrics(a, &io);
	if (io.emitted_records != 16 || io.failed_records)
		rc = -1;
	if (logger_destroy_status(b))
		rc = -1;
	if (logger_destroy_status(a))
		rc = -1;
	if (rc) {
		perror("logger drain");
		return 3;
	}

	audit_config_t ac = AUDIT_DEFAULT_CONFIG();
	ac.log_dir = ".";
	ac.name = "installed";
	ac.rotation.mode = LOGGER_ROTATE_NONE;
	if (audit_init(&ac)) {
		perror("audit_init");
		return 4;
	}
	audit_event_t event;
	memset(&event, 0, sizeof(event));
	event.event = "INSTALLED_CONSUMER";
	event.actor = "host";
	event.source = "example";
	event.resource = "test-object";
	event.operation = "check";
	if (audit_begin(&event) || audit_end(&event, AUDIT_SUCCESS, 0)) {
		audit_shutdown();
		return 5;
	}
	if (audit_shutdown_status() || audit_verify_file("installed.audit.log"))
		return 6;
	if (strcmp(audit_crypto_backend(), "builtin"))
		return 7;
	console_config_t cc = CONSOLE_DEFAULT_CONFIG();
	cc.color = CONSOLE_COLOR_NEVER;
	console_init(&cc);
	if (console_print("installed Logger/Audit/Console consumer passed"))
		return 8;
	return 0;
}
