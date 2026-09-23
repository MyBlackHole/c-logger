#define _GNU_SOURCE
#include "audit_support.h"
#include "logger_internal.h"
static int selected = -1, armed;
logger_t *__real_logger_create_reserved_file(const logger_config_t *,
					     logger_file_t *);
int __real_close(int);
logger_t *__wrap_logger_create_reserved_file(const logger_config_t *c,
					     logger_file_t *f)
{
	logger_t *l = __real_logger_create_reserved_file(c, f);
	if (l)
		selected = l->file_backend.fd;
	return l;
}
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (armed && fd == selected) {
		armed = 0;
		errno = EIO;
		return -1;
	}
	return rc;
}
int main(void)
{
	char d[] = "/tmp/file-audit-close-XXXXXX";
	enter_temp(d);
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0 && selected >= 0);
	armed = 1;
	CHECK(audit_shutdown_status() == -1 && errno == EIO);
	audit_status_t status;
	CHECK(audit_get_status(&status) == 0);
	CHECK(status.state == AUDIT_STATE_IDLE && status.error_code == EIO);
	CHECK(!armed && logger_process_object_count() == 0);
	CHECK(audit_init(&c) == 0 && audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
	audit_test_cleanup(d);
	return 0;
}
