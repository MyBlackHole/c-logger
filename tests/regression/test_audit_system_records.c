#define _GNU_SOURCE
#include "record_support.h"
#include <fcntl.h>

static const char other_instance[] = "fedcba9876543210fedcba9876543210";
static const char archive[] = "app.audit.20261010T120000.000001Z.log";
static record_fixture_t records[3];

static int same(const char *a, const char *b)
{
	return !strcmp(a, b);
}

/* The returned fixture has correct canonical encoding and digest even when
 * its fixed system metadata is deliberately forged. No parser-only failures
 * are used to prove the semantic checks. */
static void alter(record_fixture_t *r, const char *kind, int is_start)
{
	if (same(kind, "actor"))
		record_replace(r->data, sizeof(r->data),
			       " actor=\"system\"", " actor=\"outsider\"");
	else if (same(kind, "source"))
		record_replace(r->data, sizeof(r->data),
			       " source=\"local\"", " source=\"remote\"");
	else if (same(kind, "resource")) {
		if (is_start)
			record_replace(r->data, sizeof(r->data),
				       " resource=\"app\"", " resource=\"bad/path\"");
		else
			record_replace(r->data, sizeof(r->data),
				       " resource=\"audit\"", " resource=\"app\"");
	} else if (same(kind, "empty-resource"))
		record_replace(r->data, sizeof(r->data),
			       " resource=\"app\"", " resource=\"\"");
	else if (same(kind, "operation"))
		record_replace(r->data, sizeof(r->data),
			       is_start ? " operation=\"audit_start\"" :
				          " operation=\"audit_stop\"",
			       " operation=\"modify\"");
	else if (same(kind, "txn"))
		record_replace(r->data, sizeof(r->data), " txn=0", " txn=3");
	else if (same(kind, "failure"))
		record_replace(r->data, sizeof(r->data),
			       " result=SUCCESS", " result=FAILURE");
	else if (same(kind, "error"))
		record_replace(r->data, sizeof(r->data), " error=0", " error=5");
	else if (same(kind, "detail"))
		record_replace(r->data, sizeof(r->data),
			       " prev=", " detail=\"fake\" prev=");
	else if (same(kind, "empty-detail"))
		record_replace(r->data, sizeof(r->data),
			       " prev=", " detail=\"\" prev=");
	else if (same(kind, "attempt")) {
		record_replace(r->data, sizeof(r->data),
			       " phase=RESULT", " phase=ATTEMPT");
		record_replace(r->data, sizeof(r->data),
			       " result=SUCCESS error=0", "");
	} else
		CHECK(!"unknown forged system field");

	record_rehash(r->data, AUDIT_INTEGRITY_SHA256);
	r->length = strlen(r->data);
	audit_record_view_t view;
	CHECK(audit_record_parse_line(r->data, r->length, &view) == 0);
	memcpy(r->hash, view.hash, sizeof(r->hash));
}

static void run_case(const char *mode)
{
	const unsigned char zero[32] = { 0 };
	size_t count = 0, split = 0;
	int good = 0, anchored = 0;
	if (!strncmp(mode, "start-", 6)) {
		const char *kind = mode + 6;
		unsigned seq = same(kind, "seq2") ? 2u : 1u;
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, seq,
					    zero, "AUDIT_START");
		if (!same(kind, "seq2"))
			alter(&records[0], kind, 1);
		count = 1;
	} else if (!strncmp(mode, "stop-", 5)) {
		const char *kind = mode + 5;
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, 1,
					    zero, "AUDIT_START");
		unsigned seq = same(kind, "seq1") ? 1u : 2u;
		records[1] = record_fixture(AUDIT_INTEGRITY_SHA256, seq,
					    records[0].hash, "AUDIT_STOP");
		if (!same(kind, "seq1"))
			alter(&records[1], kind, 0);
		count = 2;
	} else if (same(mode, "ordinary-genesis")) {
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, 1, zero,
					    "EVENT");
		count = 1;
	} else if (same(mode, "valid-stop") ||
		   same(mode, "after-stop") || same(mode, "repeat-stop") ||
		   same(mode, "restart-after-stop") ||
		   same(mode, "restart-after-stop-cross") ||
		   same(mode, "after-stop-cross")) {
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, 1, zero,
					    "AUDIT_START");
		records[1] = record_fixture(AUDIT_INTEGRITY_SHA256, 2,
					    records[0].hash, "AUDIT_STOP");
		count = 2;
		good = same(mode, "valid-stop") ||
		       same(mode, "restart-after-stop") ||
		       same(mode, "restart-after-stop-cross");
		if (!same(mode, "valid-stop")) {
			const int fresh = same(mode, "restart-after-stop") ||
					  same(mode, "restart-after-stop-cross");
			records[2] = record_fixture_instance(
				AUDIT_INTEGRITY_SHA256, fresh ? 1u : 3u,
				records[1].hash, fresh ? "AUDIT_START" :
				same(mode, "repeat-stop") ? "AUDIT_STOP" : "EVENT",
				fresh ? other_instance : record_instance);
			count = 3;
		}
		split = (same(mode, "restart-after-stop-cross") ||
			 same(mode, "after-stop-cross")) ? 2u : 0;
	} else if (same(mode, "restart-without-stop")) {
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, 1, zero,
					    "AUDIT_START");
		records[1] = record_fixture(AUDIT_INTEGRITY_SHA256, 2,
					    records[0].hash, "EVENT");
		records[2] = record_fixture_instance(
			AUDIT_INTEGRITY_SHA256, 1, records[1].hash,
			"AUDIT_START", other_instance);
		count = 3;
		good = 1;
	} else if (same(mode, "anchored-stop-restart")) {
		/* A retired log segment may legitimately start at an old STOP,
		 * provided the persisted checkpoint anchors that very record. */
		records[0] = record_fixture(AUDIT_INTEGRITY_SHA256, 7, zero,
					    "AUDIT_STOP");
		records[1] = record_fixture_instance(
			AUDIT_INTEGRITY_SHA256, 1, records[0].hash,
			"AUDIT_START", other_instance);
		count = 2;
		good = 1;
		anchored = 1;
		split = 1;
	} else {
		CHECK(!"unknown system semantics scenario");
	}

	/* Each fixture independently passes both grammar and SHA-256 checks.
	 * Any test failure below must come from cross-record system semantics. */
	for (size_t i = 0; i < count; ++i) {
		audit_record_view_t view;
		unsigned char actual[32];
		CHECK(audit_record_parse_line(records[i].data,
						 records[i].length, &view) == 0);
		CHECK(audit_record_verify(
			&view, i ? records[i - 1u].hash : zero,
			audit_digest_provider(AUDIT_INTEGRITY_SHA256),
			actual) == 0);
		CHECK(!memcmp(actual, records[i].hash, sizeof(actual)));
	}

	if (split) {
		record_series(archive, records, 0, split);
		record_series("app.audit.log", records, split, count - split);
	} else
		record_series("app.audit.log", records, 0, count);
	record_series("whole.log", records, 0, count);

	int dirfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(dirfd >= 0);
	audit_ckpt_t cp = { .algorithm = AUDIT_INTEGRITY_SHA256 };
	if (anchored) {
		cp.seq = 7;
		cp.offset = records[0].length;
		memcpy(cp.hash, records[0].hash, 32);
	}
	audit_ckpt_t original = cp;
	crypto_file_t active = crypto_snapshot("app.audit.log");
	crypto_file_t whole = crypto_snapshot("whole.log");
	int recovered = audit_recover_set_at(dirfd, "app", "app.audit.log", &cp);
	int recovery_errno = errno;
	if (good) {
		audit_record_view_t end;
		CHECK(audit_record_parse_line(records[count - 1u].data,
						 records[count - 1u].length, &end) == 0);
		CHECK(recovered == 0 && cp.seq == end.seq &&
		      cp.offset == file_size("app.audit.log") &&
		      !memcmp(cp.hash, records[count - 1u].hash, 32));
		audit_ckpt_t retry = cp;
		CHECK(audit_recover_set_at(dirfd, "app", "app.audit.log",
						 &retry) == 0);
		CHECK(!memcmp(&cp, &retry, sizeof(cp)));
	} else {
		CHECK(recovered == -1 && recovery_errno == EBADMSG);
		CHECK(!memcmp(&cp, &original, sizeof(cp)));
	}
	crypto_same_file("app.audit.log", &active);
	crypto_same_file("whole.log", &whole);

	int offline = audit_verify_file("whole.log");
	if (good && !anchored)
		CHECK(offline == 0);
	else
		CHECK(offline == -1 && errno == EBADMSG);

	char final[65];
	memset(final, 'Q', sizeof(final));
	if (!anchored && !good) {
		CHECK(audit_verify_file_from("whole.log", NULL, final) == -1 &&
		      errno == EBADMSG);
		for (size_t i = 0; i < sizeof(final); ++i)
			CHECK(final[i] == 'Q');
	}
	if (anchored) {
		char previous[65], expected[65];
		audit_hash_hex(zero, previous);
		audit_hash_hex(records[count - 1u].hash, expected);
		CHECK(audit_verify_file_from("whole.log", previous, final) == 0);
		CHECK(!strcmp(final, expected));
	}

	free(active.data);
	free(whole.data);
	CHECK(close(dirfd) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-system-XXXXXX";
	enter_temp(dir);
	run_case(argv[1]);
	audit_test_cleanup(dir);
	printf("audit system records %s passed\n", argv[1]);
	return 0;
}
