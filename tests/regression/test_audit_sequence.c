#define _GNU_SOURCE
#include "record_support.h"
#include <errno.h>
#include <fcntl.h>

/* Deliberately independent from the digest of the preceding session. An
 * instance transition is valid only for AUDIT_START with seq=1, while hash
 * continuity is still mandatory across the same transition. */
static const char next_instance[] = "fedcba9876543210fedcba9876543210";
static const char archive_a[] = "app.audit.20260922T120000.000001Z.log";
static const char archive_b[] = "app.audit.20260922T110000.000002Z.log";
static record_fixture_t records[4];

static int is_mode(const char *actual, const char *expected)
{
	return !strcmp(actual, expected);
}

static void make_records(const char *mode)
{
	const unsigned char zero[32] = { 0 };
	uint64_t seq[4] = { 1, 2, 3, 4 };
	const char *instance[4] = {
		record_instance, record_instance, record_instance, record_instance
	};
	const char *event[4] = { "EVENT", "EVENT", "EVENT", "EVENT" };

	if (is_mode(mode, "valid-restart-cross") ||
	    is_mode(mode, "switch-no-start") ||
	    is_mode(mode, "switch-not-one")) {
		instance[2] = next_instance;
		instance[3] = next_instance;
		seq[2] = is_mode(mode, "switch-not-one") ? 2 : 1;
		seq[3] = seq[2] + 1;
		if (!is_mode(mode, "switch-no-start"))
			event[2] = "AUDIT_START";
	} else if (is_mode(mode, "valid-restart-within") ||
		   is_mode(mode, "switch-within-no-start")) {
		for (size_t i = 1; i < 4; ++i)
			instance[i] = next_instance;
		seq[1] = 1;
		seq[2] = 2;
		seq[3] = 3;
		if (!is_mode(mode, "switch-within-no-start"))
			event[1] = "AUDIT_START";
	} else if (is_mode(mode, "gap-within")) {
		seq[1] = 3;
		seq[2] = 4;
		seq[3] = 5;
	} else if (is_mode(mode, "duplicate-within")) {
		seq[1] = 1;
		seq[2] = 2;
		seq[3] = 3;
	} else if (is_mode(mode, "gap-cross")) {
		seq[2] = 4;
		seq[3] = 5;
	} else if (is_mode(mode, "duplicate-cross")) {
		seq[2] = 2;
		seq[3] = 3;
	} else if (is_mode(mode, "same-id-reset")) {
		seq[2] = 1;
		seq[3] = 2;
		event[2] = "AUDIT_START";
	} else if (is_mode(mode, "start-not-one")) {
		event[1] = "AUDIT_START";
	} else if (is_mode(mode, "genesis-not-one")) {
		seq[0] = 2;
		seq[1] = 3;
		seq[2] = 4;
		seq[3] = 5;
	} else if (is_mode(mode, "anchored-suffix")) {
		seq[0] = 7;
		seq[1] = 8;
		seq[2] = 9;
		seq[3] = 10;
	} else if (is_mode(mode, "wrap")) {
		seq[0] = UINT64_MAX - 1u;
		seq[1] = UINT64_MAX;
		seq[2] = 1;
		seq[3] = 2;
	} else {
		CHECK(is_mode(mode, "valid-rotation") ||
		      is_mode(mode, "valid-genesis"));
	}

	for (size_t i = 0; i < 4; ++i) {
		records[i] = record_fixture_instance(
			AUDIT_INTEGRITY_SHA256, seq[i],
			i ? records[i - 1].hash : zero, event[i], instance[i]);
		/* Independent proof that every altered record has a valid chain
		 * digest: the negative tests are semantic, not hash failures. */
		audit_record_view_t view;
		unsigned char digest[32];
		CHECK(audit_record_parse_line(records[i].data,
						 records[i].length, &view) == 0);
		CHECK(audit_record_verify(
			&view, i ? records[i - 1].hash : zero,
			audit_digest_provider(AUDIT_INTEGRITY_SHA256), digest) == 0);
		CHECK(!memcmp(digest, records[i].hash, sizeof(digest)));
	}
}

static void run_case(const char *mode)
{
	make_records(mode);
	const int good = is_mode(mode, "valid-rotation") ||
			 is_mode(mode, "valid-genesis") ||
			 is_mode(mode, "valid-restart-cross") ||
			 is_mode(mode, "valid-restart-within") ||
			 is_mode(mode, "anchored-suffix");
	const int genesis = is_mode(mode, "valid-genesis") ||
			    is_mode(mode, "genesis-not-one");
	const int suffix = is_mode(mode, "anchored-suffix") ||
			   is_mode(mode, "wrap");

	record_series(archive_a, records, 0, 2);
	record_series(archive_b, records, 2, 1);
	record_series("app.audit.log", records, 3, 1);
	record_series("combined.log", records, 0, 4);
	int dirfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(dirfd >= 0);

	const unsigned char zero[32] = { 0 };
	audit_ckpt_t cp = { .algorithm = AUDIT_INTEGRITY_SHA256 };
	if (!genesis) {
		cp.seq = records[0].length ? 1u : 0u;
		/* The retained suffix is anchored by an explicit checkpoint, not
		 * by a presumed genesis sequence. */
		audit_record_view_t first;
		CHECK(audit_record_parse_line(records[0].data,
						 records[0].length, &first) == 0);
		cp.seq = first.seq;
		cp.offset = records[0].length;
		memcpy(cp.hash, records[0].hash, sizeof(cp.hash));
	}
	audit_ckpt_t before = cp;
	crypto_file_t active = crypto_snapshot("app.audit.log");
	crypto_file_t combined = crypto_snapshot("combined.log");

	int recovery = audit_recover_set_at(dirfd, "app", "app.audit.log", &cp);
	int recovery_errno = errno;
	if (good) {
		CHECK(recovery == 0 && cp.offset == records[3].length &&
		      !memcmp(cp.hash, records[3].hash, sizeof(cp.hash)));
		audit_record_view_t last;
		CHECK(audit_record_parse_line(records[3].data,
						 records[3].length, &last) == 0);
		CHECK(cp.seq == last.seq);
		audit_ckpt_t again = cp;
		CHECK(audit_recover_set_at(dirfd, "app", "app.audit.log",
						 &again) == 0);
		CHECK(!memcmp(&cp, &again, sizeof(cp)));
	} else {
		CHECK(recovery == -1 && recovery_errno == EBADMSG);
		CHECK(!memcmp(&cp, &before, sizeof(cp)));
	}
	crypto_same_file("app.audit.log", &active);
	crypto_same_file("combined.log", &combined);

	int verified = audit_verify_file("combined.log");
	if (good && !suffix)
		CHECK(verified == 0);
	else
		CHECK(verified == -1 && errno == EBADMSG);

	if (suffix) {
		char zero_hex[65], final_hex[65] = { 0 };
		audit_hash_hex(zero, zero_hex);
		int anchored = audit_verify_file_from("combined.log", zero_hex,
						      final_hex);
		if (good) {
			char expected[65];
			audit_hash_hex(records[3].hash, expected);
			CHECK(anchored == 0 && !strcmp(final_hex, expected));
		} else
			CHECK(anchored == -1 && errno == EBADMSG);
	}
	free(active.data);
	free(combined.data);
	CHECK(close(dirfd) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-sequence-XXXXXX";
	enter_temp(dir);
	run_case(argv[1]);
	audit_test_cleanup(dir);
	printf("audit sequence %s passed\n", argv[1]);
	return 0;
}
