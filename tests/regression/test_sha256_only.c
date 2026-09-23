#define _GNU_SOURCE
#include "crypto_support.h"
#include "audit_record.h"

/* Retired wire/API ID. It deliberately has no public enum name now. The
 * historical fixture is inert negative-test data, not a supported algorithm. */
#define RETIRED_ID ((audit_integrity_t)2)

static void copy_fixture(const char *directory, const char *name)
{
	char path[PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
	CHECK(n > 0 && (size_t)n < sizeof(path));
	crypto_file_t saved = crypto_snapshot(path);
	crypto_restore(name, &saved);
	free(saved.data);
}

static void only_provider(void)
{
	CHECK(AUDIT_INTEGRITY_NONE == 0 && AUDIT_INTEGRITY_SHA256 == 1);
	audit_config_t c = AUDIT_DEFAULT_CONFIG();
	CHECK(c.integrity == AUDIT_INTEGRITY_SHA256);
	const audit_digest_ops_t *d = audit_digest_provider(c.integrity);
	CHECK(d && d->algorithm == c.integrity && d->digest_size == 32);
	CHECK(audit_digest_check(d) == 0);
	CHECK(!strcmp(audit_crypto_backend(), "builtin"));
	const int unsupported[] = { 0, 2, 3, -1, INT_MAX };
	for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]);
	     ++i)
		CHECK(audit_digest_provider(
			      (audit_integrity_t)unsupported[i]) == NULL);
}

static void config_rejected(void)
{
	audit_config_t c = audit_test_config();
	const int unsupported[] = { 2, 3, -1, INT_MAX };
	for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]);
	     ++i) {
		c.integrity = (audit_integrity_t)unsupported[i];
		crypto_expect_error(audit_init(&c), EPROTONOSUPPORT);
		/* Invalid algorithm rejection precedes locks/files or any output. */
		DIR *dir = opendir(".");
		CHECK(dir);
		struct dirent *e;
		while ((e = readdir(dir)))
			CHECK(!strcmp(e->d_name, ".") ||
			      !strcmp(e->d_name, ".."));
		CHECK(closedir(dir) == 0);
		audit_status_t status;
		CHECK(audit_get_status(&status) == 0);
		CHECK(status.state == AUDIT_STATE_IDLE &&
		      status.error_code == EPROTONOSUPPORT);
	}
	c.integrity = AUDIT_INTEGRITY_SHA256;
	CHECK(audit_init(&c) == 0 && audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
}

static void verify_rejected(int empty)
{
	const char *path = empty ? "empty.log" : "does-not-exist.log";
	if (empty) {
		FILE *f = fopen(path, "w");
		CHECK(f);
		CHECK(fclose(f) == 0);
		CHECK(audit_verify_file(path) ==
		      0); /* permitted empty SHA-256 stream */
	}
	crypto_expect_error(audit_verify_file_with(path, RETIRED_ID),
			    EPROTONOSUPPORT);
	crypto_expect_error(audit_verify_file_with(path, AUDIT_INTEGRITY_NONE),
			    EPROTONOSUPPORT);
	if (empty)
		CHECK(file_size(path) == 0);
	else
		CHECK(access(path, F_OK) == -1 && errno == ENOENT);
}

static void rejected_history(const char *mode, const char *fixture)
{
	copy_fixture(fixture, "app.audit.log");
	copy_fixture(fixture, "app.audit.state");
	crypto_file_t log = crypto_snapshot("app.audit.log");
	crypto_file_t state = crypto_snapshot("app.audit.state");
	if (!strcmp(mode, "verify-history")) {
		crypto_expect_error(audit_verify_file_with("app.audit.log",
							   RETIRED_ID),
				    EPROTONOSUPPORT);
		crypto_expect_error(audit_verify_file("app.audit.log"),
				    EBADMSG);
		char final[65], before[65];
		memset(final, 'x', sizeof(final));
		memcpy(before, final, sizeof(before));
		crypto_expect_error(audit_verify_file_from("app.audit.log",
							   NULL, final),
				    EBADMSG);
		CHECK(!memcmp(final, before, sizeof(final)));
	} else if (!strcmp(mode, "checkpoint-read")) {
		audit_ckpt_t out, before;
		memset(&out, 0xa5, sizeof(out));
		memcpy(&before, &out, sizeof(before));
		crypto_expect_error(audit_checkpoint_load("app.audit.state",
							  &out),
				    EBADMSG);
		CHECK(!memcmp(&out, &before, sizeof(out)));
	} else if (!strcmp(mode, "recover-id")) {
		audit_ckpt_t out, before;
		memset(&out, 0, sizeof(out));
		out.algorithm = (uint32_t)RETIRED_ID;
		memcpy(&before, &out, sizeof(before));
		crypto_expect_error(audit_recover_set(".", "app",
						      "app.audit.log", &out),
				    EPROTONOSUPPORT);
		CHECK(!memcmp(&out, &before, sizeof(out)));
	} else if (!strcmp(mode, "checkpoint-write")) {
		audit_ckpt_t cp = { .algorithm = (uint32_t)RETIRED_ID };
		crypto_expect_error(audit_checkpoint_persist("app.audit.state",
							     &cp),
				    EINVAL);
	} else {
		int missing = !strcmp(mode, "init-missing-state");
		int relabel = !strcmp(mode, "init-relabel-state");
		CHECK(missing || relabel || !strcmp(mode, "init-history"));
		if (missing)
			CHECK(unlink("app.audit.state") == 0);
		if (relabel) {
			/* Even recomputing a local CRC cannot turn a retired digest chain
             * into SHA-256. Full recovery verification must reject it. */
			char changed[512];
			CHECK(state.size < sizeof(changed));
			memcpy(changed, state.data, state.size);
			changed[state.size] = '\0';
			char *alg = strstr(changed, "alg=2");
			CHECK(alg);
			alg[4] = '1';
			char *crc = strstr(changed, " crc=");
			CHECK(crc);
			char digits[9];
			CHECK(snprintf(digits, sizeof(digits), "%08x",
				       audit_crc32(changed,
						   (size_t)(crc - changed))) ==
			      8);
			memcpy(crc + 5, digits, 8);
			memcpy(state.data, changed, state.size);
			crypto_restore("app.audit.state", &state);
		}
		audit_config_t c = audit_test_config();
		crypto_expect_error(audit_init(&c), EBADMSG);
		audit_status_t status;
		CHECK(audit_get_status(&status) == 0);
		CHECK(status.state == AUDIT_STATE_IDLE &&
		      status.committed_seq == 0);
		if (missing) {
			CHECK(access("app.audit.state", F_OK) == -1 &&
			      errno == ENOENT);
			/* Restore for the identical-byte assertion below; the library
             * must not have created a replacement checkpoint on failure. */
			crypto_restore("app.audit.state", &state);
		}
		CHECK(count_event("AUDIT_START") ==
		      1); /* no new lifecycle appended */
	}
	crypto_same_file("app.audit.log", &log);
	crypto_same_file("app.audit.state", &state);
	free(log.data);
	free(state.data);
}

static void live_unchanged(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);
	crypto_file_t log = crypto_snapshot("app.audit.log");
	crypto_file_t state = crypto_snapshot("app.audit.state");
	crypto_expect_error(audit_verify_file_with("app.audit.log", RETIRED_ID),
			    EPROTONOSUPPORT);
	crypto_same_file("app.audit.log", &log);
	crypto_same_file("app.audit.state", &state);
	audit_event_t e = audit_test_event("SHA_ONLY_STILL_RUNNING");
	CHECK(audit_write(&e) == 0 && audit_shutdown_status() == 0);
	CHECK(audit_verify_file("app.audit.log") == 0);
	free(log.data);
	free(state.data);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2 || argc == 3);
	char directory[] = "/tmp/logger-sha256-only-XXXXXX";
	enter_temp(directory);
	if (!strcmp(argv[1], "provider"))
		only_provider();
	else if (!strcmp(argv[1], "config"))
		config_rejected();
	else if (!strcmp(argv[1], "verify-empty"))
		verify_rejected(1);
	else if (!strcmp(argv[1], "verify-missing"))
		verify_rejected(0);
	else if (!strcmp(argv[1], "live"))
		live_unchanged();
	else {
		CHECK(argc == 3);
		rejected_history(argv[1], argv[2]);
	}
	audit_test_cleanup(directory);
	printf("SHA-256-only %s passed\n", argv[1]);
	return 0;
}
