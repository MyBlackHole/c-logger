#define _GNU_SOURCE
#include "audit_support.h"
#include <sys/random.h>

static int entropy_mode;
static unsigned calls;

ssize_t __wrap_getrandom(void *buf, size_t len, unsigned flags)
{
	CHECK(flags == GRND_NONBLOCK);
	++calls;
	if (!entropy_mode) {
		errno = EAGAIN;
		return -1;
	}
	if (entropy_mode == 1) {
		entropy_mode = 2;
		errno = EINTR;
		return -1;
	}
	memset(buf, 0x5a, len);
	return (ssize_t)len;
}

int main(void)
{
	char dir[] = "/tmp/audit-entropy-XXXXXX";
	enter_temp(dir);
	audit_config_t c = audit_test_config();

	/* CRNG-not-ready is a pre-resource fail-fast boundary. */
	entropy_mode = 0;
	calls = 0;
	errno = 0;
	CHECK(audit_init(&c) == -1 && errno == EAGAIN);
	CHECK(calls == 1);
	CHECK(access("app.audit.log", F_OK) == -1 && errno == ENOENT);
	CHECK(access("app.audit.log.logger.lock", F_OK) == -1 && errno == ENOENT);
	CHECK(access("app.audit.state", F_OK) == -1 && errno == ENOENT);
	CHECK(access("app.audit.state.logger.lock", F_OK) == -1 && errno == ENOENT);

	/* EINTR remains retryable; a later ready CRNG must initialize normally. */
	entropy_mode = 1;
	calls = 0;
	CHECK(audit_init(&c) == 0);
	CHECK(calls == 2);
	char id[33];
	CHECK(audit_instance_id_copy(id) == 0);
	CHECK(strlen(id) == 32);
	CHECK(audit_shutdown_status() == 0);

	audit_test_cleanup(dir);
	return 0;
}
