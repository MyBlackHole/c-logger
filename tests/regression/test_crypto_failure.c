#define _GNU_SOURCE
#include "crypto_support.h"

/* Failure providers are supplied only by this test's link-time wrapper.
 * Production code has no new env switch, callback setter, or fault hook. */
const audit_digest_ops_t *__real_audit_digest_provider(audit_integrity_t);
static const audit_digest_ops_t *real_sha;
static audit_digest_ops_t sha;
static const char *wanted;
static int probe_failure;
static int failure_error = EIO;
static unsigned matching_calls, failure_at = 1;
static unsigned hash_calls;

static int hash_call(const audit_digest_ops_t *real, const void *p, size_t n,
		     unsigned char out[32])
{
	++hash_calls;
	int match = probe_failure && !n;
	if (wanted && n && memmem(p, n, wanted, strlen(wanted)))
		match = 1;
	if (match && ++matching_calls == failure_at)
		return -failure_error;
	return real->hash(p, n, out);
}
static int sha_hash(const void *p, size_t n, unsigned char out[32])
{
	return hash_call(real_sha, p, n, out);
}
const audit_digest_ops_t *
__wrap_audit_digest_provider(audit_integrity_t algorithm)
{
	if (algorithm == AUDIT_INTEGRITY_SHA256)
		return &sha;
	return __real_audit_digest_provider(algorithm);
}
static void disarm(void)
{
	wanted = NULL;
	probe_failure = 0;
	matching_calls = 0;
}
static void arm_record(const char *needle, unsigned at)
{
	disarm();
	wanted = needle;
	failure_at = at;
	failure_error = EIO;
}
static void arm_probe(void)
{
	disarm();
	probe_failure = 1;
	failure_at = 1;
	failure_error = ENOMEM;
}
static void verify_good(audit_integrity_t algorithm)
{
	CHECK(audit_verify_file_with("app.audit.log", algorithm) == 0);
}

static void init_failure(audit_config_t *config, int preflight)
{
	if (preflight)
		arm_probe();
	else
		arm_record("event=\"AUDIT_START\"", 1);
	crypto_expect_error(audit_init(config), failure_error);
	CHECK(matching_calls == 1);
	audit_status_t status;
	CHECK(audit_get_status(&status) == 0);
	CHECK(status.state == AUDIT_STATE_IDLE &&
	      status.error_code == failure_error);
	CHECK(status.committed_seq == 0);
	if (preflight) {
		CHECK(access("app.audit.log", F_OK) == -1 && errno == ENOENT);
		CHECK(access("app.audit.state", F_OK) == -1 && errno == ENOENT);
		CHECK(access("app.audit.lock", F_OK) == -1 && errno == ENOENT);
	} else {
		CHECK(file_size("app.audit.log") == 0);
		check_process_lock("free");
	}
	disarm();
	CHECK(audit_init(config) == 0);
	CHECK(audit_shutdown_status() == 0);
	verify_good(config->integrity);
}

static void runtime_failure(audit_config_t *config, const char *mode)
{
	CHECK(audit_init(config) == 0);
	audit_event_t e = audit_test_event("PAYLOAD");
	if (!strcmp(mode, "end"))
		CHECK(audit_begin(&e) == 0);
	audit_status_t before, after;
	CHECK(audit_get_status(&before) == 0);
	crypto_file_t log = crypto_snapshot("app.audit.log");
	crypto_file_t state = crypto_snapshot("app.audit.state");
	arm_record(!strcmp(mode, "stop") ? "event=\"AUDIT_STOP\"" :
					   "event=\"PAYLOAD\"",
		   1);
	int rc;
	if (!strcmp(mode, "write"))
		rc = audit_write(&e);
	else if (!strcmp(mode, "begin"))
		rc = audit_begin(&e);
	else if (!strcmp(mode, "end"))
		rc = audit_end(&e, AUDIT_SUCCESS, 0);
	else {
		CHECK(!strcmp(mode, "stop"));
		rc = audit_shutdown_status();
	}
	crypto_expect_error(rc, EIO);
	CHECK(matching_calls == 1);
	CHECK(audit_get_status(&after) == 0);
	CHECK(after.state == (!strcmp(mode, "stop") ?
				      AUDIT_STATE_IDLE :
				      AUDIT_STATE_CRYPTO_FAILED));
	CHECK(after.error_code == EIO && !after.checkpoint_dirty);
	CHECK(after.committed_seq == before.committed_seq);
	CHECK(after.checkpoint_seq == before.checkpoint_seq);
	crypto_same_file("app.audit.log", &log);
	crypto_same_file("app.audit.state", &state);
	disarm();
	if (strcmp(mode, "stop")) {
		unsigned previous_calls = hash_calls;
		audit_event_t next = audit_test_event("MUST_NOT_APPEAR");
		next.transaction_id = 123;
		crypto_expect_error(audit_write(&next), EIO);
		crypto_expect_error(audit_begin(&next), EIO);
		crypto_expect_error(audit_end(&next, AUDIT_SUCCESS, 0), EIO);
		crypto_expect_error(audit_flush(), EIO);
		CHECK(hash_calls == previous_calls); /* no auto-retry */
		crypto_expect_error(audit_shutdown_status(), EIO);
		CHECK(hash_calls == previous_calls); /* no fake STOP */
	}
	crypto_same_file("app.audit.log", &log);
	crypto_same_file("app.audit.state", &state);
	check_process_lock("free");
	CHECK(audit_init(config) == 0);
	CHECK(audit_write(&e) == 0);
	CHECK(audit_shutdown_status() == 0);
	verify_good(config->integrity);
	free(log.data);
	free(state.data);
}

static void verify_failure(audit_config_t *config, int empty)
{
	if (empty) {
		FILE *f = fopen("app.audit.log", "wb");
		CHECK(f && fclose(f) == 0);
	} else {
		CHECK(audit_init(config) == 0);
		audit_event_t e = audit_test_event("PAYLOAD");
		CHECK(audit_write(&e) == 0);
		CHECK(audit_shutdown_status() == 0);
	}
	crypto_file_t log = crypto_snapshot("app.audit.log");
	for (unsigned pass = 0; pass < 2; ++pass) {
		if (empty)
			arm_probe();
		else
			arm_record("instance=",
				   2); /* fail after one good record */
		int rc;
		char final[65], before[65];
		memset(final, 'Q', sizeof(final));
		memcpy(before, final, sizeof(before));
		if (pass && config->integrity == AUDIT_INTEGRITY_SHA256)
			rc = audit_verify_file_from("app.audit.log", NULL,
						    final);
		else
			rc = audit_verify_file_with("app.audit.log",
						    config->integrity);
		crypto_expect_error(rc, failure_error);
		CHECK(!memcmp(final, before, sizeof(final)));
		CHECK(matching_calls == (empty ? 1u : 2u));
		crypto_same_file("app.audit.log", &log);
	}
	disarm();
	if (!empty)
		verify_good(config->integrity);
	free(log.data);
}

static void recovery_failure(audit_config_t *config, const char *mode)
{
	CHECK(audit_init(config) == 0);
	crypto_file_t early = crypto_snapshot("app.audit.state");
	audit_event_t e = audit_test_event("PAYLOAD");
	CHECK(audit_write(&e) == 0);
	CHECK(audit_shutdown_status() == 0);
	int preflight = !strcmp(mode, "recover-probe");
	if (preflight) {
		FILE *f = fopen("app.audit.log", "ab");
		CHECK(f != NULL);
		CHECK(fputs("uncommitted tail", f) >= 0 && fclose(f) == 0);
	} else {
		crypto_restore("app.audit.state",
			       &early); /* a valid stale checkpoint */
	}
	crypto_file_t log = crypto_snapshot("app.audit.log");
	crypto_file_t state = crypto_snapshot("app.audit.state");
	audit_ckpt_t checkpoint;
	CHECK(audit_checkpoint_load("app.audit.state", &checkpoint) == 0);
	audit_ckpt_t before = checkpoint;
	if (preflight)
		arm_probe();
	else
		arm_record("instance=", 2);
	if (!strcmp(mode, "recover-init")) {
		crypto_expect_error(audit_init(config), EIO);
		check_process_lock("free");
	} else {
		crypto_expect_error(audit_recover_set(".", "app",
						      "app.audit.log",
						      &checkpoint),
				    failure_error);
		CHECK(checkpoint.algorithm == before.algorithm &&
		      checkpoint.seq == before.seq);
		CHECK(checkpoint.offset == before.offset &&
		      !memcmp(checkpoint.hash, before.hash, 32));
	}
	CHECK(matching_calls == (preflight ? 1u : 2u));
	crypto_same_file("app.audit.log", &log);
	crypto_same_file("app.audit.state", &state);
	disarm();
	CHECK(audit_init(config) == 0);
	CHECK(audit_shutdown_status() == 0);
	verify_good(config->integrity);
	free(early.data);
	free(log.data);
	free(state.data);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--probe-lock"))
		return audit_probe_lock(argv[2]);
	CHECK(argc == 3);
	real_sha = __real_audit_digest_provider(AUDIT_INTEGRITY_SHA256);
	CHECK(real_sha);
	sha = *real_sha;
	sha.hash = sha_hash;
	char path[] = "/tmp/logger-crypto-failure-XXXXXX";
	enter_temp(path);
	audit_config_t config = audit_test_config();
	config.integrity = crypto_algorithm(argv[2]);
	const char *mode = argv[1];
	if (!strcmp(mode, "init-probe"))
		init_failure(&config, 1);
	else if (!strcmp(mode, "init-start"))
		init_failure(&config, 0);
	else if (!strcmp(mode, "verify"))
		verify_failure(&config, 0);
	else if (!strcmp(mode, "verify-empty"))
		verify_failure(&config, 1);
	else if (!strncmp(mode, "recover-", 8))
		recovery_failure(&config, mode);
	else if (!strcmp(mode, "none")) {
		config.integrity = AUDIT_INTEGRITY_NONE;
		arm_probe();
		CHECK(audit_init(&config) == 0);
		audit_event_t e = audit_test_event("NO_INTEGRITY_REQUESTED");
		CHECK(audit_write(&e) == 0);
		CHECK(audit_shutdown_status() == 0 && hash_calls == 0);
		CHECK(!file_contains("app.audit.log", " hash="));
	} else
		runtime_failure(&config, mode);
	audit_test_cleanup(path);
	printf("%s %s %s passed\n", audit_crypto_backend(), argv[2], mode);
	return 0;
}
