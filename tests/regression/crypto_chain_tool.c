#include "crypto_support.h"

/* Invoked by scripts/crypto_cross_version.py in a fresh private directory. */
int main(int argc, char **argv)
{
	CHECK(argc == 3);
	audit_config_t config = audit_test_config();
	config.integrity = crypto_algorithm(argv[2]);
	if (!strcmp(argv[1], "append")) {
		CHECK(audit_init(&config) == 0);
		audit_event_t event = audit_test_event("CROSS_VERSION");
		event.detail = audit_crypto_backend();
		CHECK(audit_write(&event) == 0);
		CHECK(audit_shutdown_status() == 0);
	} else
		CHECK(!strcmp(argv[1], "verify"));
	CHECK(audit_verify_file_with("app.audit.log", config.integrity) == 0);
	printf("%s %s %s OK\n", audit_crypto_backend(), argv[2], argv[1]);
	return 0;
}
