#define _GNU_SOURCE
#include "record_support.h"

static void roundtrip(audit_integrity_t alg)
{
	audit_config_t c = audit_test_config();
	c.integrity = alg;
	CHECK(audit_init(&c) == 0);
	char controls[160];
	size_t k = 0;
	for (unsigned i = 1; i < 32; ++i)
		controls[k++] = (char)i;
	controls[k++] = 127;
	const char *suffix =
		" prev=hash hash=prev \"\\ \xe4\xb8\xad\xe6\x96\x87";
	strcpy(controls + k, suffix);
	audit_event_t e = audit_test_event("EV prev=123 hash=456 instance=789");
	e.actor = controls;
	e.source = controls;
	e.resource = controls;
	e.operation = controls;
	e.detail = controls;
	CHECK(audit_begin(&e) == 0);
	CHECK(audit_end(&e, AUDIT_FAILURE, INT_MIN) == 0);
	e.error_code = INT_MAX;
	e.detail = "";
	e.actor = NULL;
	CHECK(audit_write(&e) == 0);
	CHECK(audit_shutdown_status() == 0);
	CHECK(audit_verify_file_with("app.audit.log", alg) == 0);
	FILE *f = fopen("app.audit.log", "rb");
	CHECK(f);
	char line[AUDIT_RECORD_MAX + 1u];
	size_t n;
	unsigned count = 0;
	for (;;) {
		int kind = audit_line_read(f, line, AUDIT_RECORD_MAX, &n);
		if (kind == AUDIT_LINE_EOF)
			break;
		CHECK(kind == AUDIT_LINE_COMPLETE);
		audit_record_view_t r;
		CHECK(audit_record_parse_line(line, n, &r) == 0);
		++count;
	}
	CHECK(count == 5 && fclose(f) == 0);
	/* Newly produced escape bytes are visible, never raw physical controls. */
	CHECK(file_contains("app.audit.log", "\\x01") &&
	      file_contains("app.audit.log", "\\x7f"));
	CHECK(audit_init(&c) == 0 && audit_shutdown_status() == 0);
	CHECK(audit_verify_file_with("app.audit.log", alg) == 0);
}

static void malformed(const char *mode, audit_integrity_t alg)
{
	const unsigned char zero[32] = { 0 };
	record_fixture_t r = record_fixture(alg, 1, zero, "EVENT");
	char line[20050];
	memcpy(line, r.data, r.length + 1u);
	size_t n;
	int expected = EBADMSG, rehash = 1;
	if (!strcmp(mode, "suffix")) {
		record_replace(line, sizeof(line), "\n", " extra=UNHASHED\n");
		rehash = 0;
	} else if (!strcmp(mode, "duplicate"))
		record_replace(line, sizeof(line), " seq=1", " seq=1 seq=2");
	else if (!strcmp(mode, "unknown"))
		record_replace(line, sizeof(line),
			       " actor=", " surprise=1 actor=");
	else if (!strcmp(mode, "reorder"))
		record_replace(line, sizeof(line),
			       "actor=\"user\" source=\"test\"",
			       "source=\"test\" actor=\"user\"");
	else if (!strcmp(mode, "escape"))
		record_replace(line, sizeof(line), "actor=\"user\"",
			       "actor=\"bad\\q\"");
	else if (!strcmp(mode, "escape-nul"))
		record_replace(line, sizeof(line), "actor=\"user\"",
			       "actor=\"bad\\x00\"");
	else if (!strcmp(mode, "escape-print"))
		record_replace(line, sizeof(line), "actor=\"user\"",
			       "actor=\"bad\\x41\"");
	else if (!strcmp(mode, "raw-control"))
		record_replace(line, sizeof(line), "actor=\"user\"",
			       "actor=\"bad\tvalue\"");
	else if (!strcmp(mode, "unclosed"))
		record_replace(line, sizeof(line), "actor=\"user\"",
			       "actor=\"user");
	else if (!strcmp(mode, "seq-zero"))
		record_replace(line, sizeof(line), " seq=1", " seq=0");
	else if (!strcmp(mode, "seq-overflow"))
		record_replace(line, sizeof(line), " seq=1",
			       " seq=18446744073709551616");
	else if (!strcmp(mode, "seq-leading"))
		record_replace(line, sizeof(line), " seq=1", " seq=01");
	else if (!strcmp(mode, "seq-negative"))
		record_replace(line, sizeof(line), " seq=1", " seq=-1");
	else if (!strcmp(mode, "txn-plus"))
		record_replace(line, sizeof(line), " txn=0", " txn=+1");
	else if (!strcmp(mode, "txn-overflow"))
		record_replace(line, sizeof(line), " txn=0",
			       " txn=18446744073709551616");
	else if (!strcmp(mode, "error-overflow"))
		record_replace(line, sizeof(line), " error=0",
			       " error=2147483648");
	else if (!strcmp(mode, "error-underflow"))
		record_replace(line, sizeof(line), " error=0",
			       " error=-2147483649");
	else if (!strcmp(mode, "negative-zero"))
		record_replace(line, sizeof(line), " error=0", " error=-0");
	else if (!strcmp(mode, "phase"))
		record_replace(line, sizeof(line), "phase=RESULT",
			       "phase=UNKNOWN");
	else if (!strcmp(mode, "result"))
		record_replace(line, sizeof(line), "result=SUCCESS",
			       "result=MAYBE");
	else if (!strcmp(mode, "attempt-result"))
		record_replace(line, sizeof(line), "phase=RESULT",
			       "phase=ATTEMPT");
	else if (!strcmp(mode, "missing-result"))
		record_replace(line, sizeof(line), " result=SUCCESS error=0",
			       "");
	else if (!strcmp(mode, "empty-event"))
		record_replace(line, sizeof(line), "event=\"EVENT\"",
			       "event=\"\"");
	else if (!strcmp(mode, "empty-operation"))
		record_replace(line, sizeof(line), "operation=\"update\"",
			       "operation=\"\"");
	else if (!strcmp(mode, "instance"))
		record_replace(line, sizeof(line), "instance=012345",
			       "instance=X12345");
	else if (!strcmp(mode, "bad-prefix")) {
		record_replace(line, sizeof(line), "[AUDIT]", "[OTHER]");
		rehash = 0;
	} else if (!strcmp(mode, "bad-date")) {
		record_replace(line, sizeof(line), "2026-09-22", "2026-02-30");
		rehash = 0;
	} else if (!strcmp(mode, "trailing-space")) {
		record_replace(line, sizeof(line), "\n", " \n");
		rehash = 0;
	} else if (!strcmp(mode, "crlf")) {
		record_replace(line, sizeof(line), "\n", "\r\n");
		rehash = 0;
	} else if (!strcmp(mode, "missing-lf")) {
		line[r.length - 1u] = 0;
		rehash = 0;
	} else if (!strcmp(mode, "short-hash")) {
		line[r.length - 2u] = '\n';
		line[r.length - 1u] = 0;
		rehash = 0;
	} else if (!strcmp(mode, "long-hash")) {
		record_replace(line, sizeof(line), "\n", "0\n");
		rehash = 0;
	} else if (!strcmp(mode, "hash-nonhex")) {
		line[r.length - 2u] = 'g';
		rehash = 0;
	} else if (!strcmp(mode, "hash-upper")) {
		line[r.length - 2u] = 'A';
		rehash = 0;
	} else if (!strcmp(mode, "oversize")) {
		memset(line, 'x', 20000);
		line[20000] = '\n';
		line[20001] = 0;
		rehash = 0;
		expected = EOVERFLOW;
	} else if (!strcmp(mode, "oversize-eof")) {
		memset(line, 'x', 20000);
		line[20000] = 0;
		rehash = 0;
		expected = EOVERFLOW;
	} else if (!strcmp(mode, "nul")) {
		rehash = 0;
	} else
		CHECK(0);
	if (rehash)
		record_rehash(line, alg);
	n = strlen(line);
	if (!strcmp(mode, "nul"))
		line[r.length - 2u] = 0;
	record_write("input.log", line, n);
	crypto_file_t snapshot = crypto_snapshot("input.log");
	crypto_expect_error(audit_verify_file_with("input.log", alg), expected);
	if (alg == AUDIT_INTEGRITY_SHA256) {
		char final[65];
		memset(final, 'Q', sizeof(final));
		crypto_expect_error(audit_verify_file_from("input.log", NULL,
							   final),
				    expected);
		for (size_t i = 0; i < sizeof(final); ++i)
			CHECK(final[i] == 'Q');
	}
	crypto_same_file("input.log", &snapshot);
	free(snapshot.data);
}

static void bounds(audit_integrity_t alg)
{
	unsigned char zero[32] = { 0 }, hash[32];
	char payload[AUDIT_PAYLOAD_MAX + 1u], big[AUDIT_PAYLOAD_MAX + 1u];
	audit_event_t e = audit_test_event("EVENT");
	e.detail = "";
	size_t empty;
	CHECK(audit_payload_encode(record_instance, 1, &e, payload,
				   sizeof(payload) - 140u, &empty) == 0);
	size_t fitting = AUDIT_PAYLOAD_MAX - 140u - empty;
	memset(big, 'x', fitting + 1u);
	big[fitting] = 0;
	e.detail = big;
	size_t n;
	CHECK(audit_payload_encode(record_instance, UINT64_MAX, &e, payload,
				   sizeof(payload) - 140u, &n) == -EOVERFLOW);
	CHECK(audit_payload_encode(record_instance, 1, &e, payload,
				   sizeof(payload) - 140u, &n) == 0);
	CHECK(n + 140u == AUDIT_PAYLOAD_MAX);
	audit_config_t c = audit_test_config();
	c.integrity = alg;
	CHECK(audit_init(&c) == 0);
	/* Runtime seq 2 has same width as 1 used to find this exact bound. */
	CHECK(audit_write(&e) == 0);
	crypto_file_t before = crypto_snapshot("app.audit.log");
	big[fitting] = 'x';
	big[fitting + 1u] = 0;
	crypto_expect_error(audit_write(&e), EOVERFLOW);
	crypto_same_file("app.audit.log", &before);
	free(before.data);
	CHECK(audit_shutdown_status() == 0 &&
	      audit_verify_file_with("app.audit.log", alg) == 0);
	record_fixture_t r = record_fixture(alg, UINT64_MAX, zero, "MAX");
	audit_record_view_t view;
	CHECK(audit_record_parse_line(r.data, r.length, &view) == 0 &&
	      view.seq == UINT64_MAX);
	CHECK(audit_record_verify(&view, zero, audit_digest_provider(alg),
				  hash) == 0);
}

static void fuzz(audit_integrity_t alg)
{
	const unsigned char zero[32] = { 0 };
	record_fixture_t r = record_fixture(alg, 1, zero, "EVENT");
	uint32_t random = 0x6a53d712;
	for (unsigned k = 0; k < 12000; ++k) {
		char buf[AUDIT_RECORD_MAX + 2u];
		memcpy(buf, r.data, r.length);
		random = random * 1664525u + 1013904223u;
		size_t length = k % 3 ? r.length : random % (r.length + 1u);
		if (length)
			buf[random % length] = (char)(random >> 24);
		audit_record_view_t view, untouched;
		memset(&view, 0x5a, sizeof(view));
		untouched = view;
		int rc = audit_record_parse_line(buf, length, &view);
		if (rc)
			CHECK(!memcmp(&view, &untouched, sizeof(view)));
		else {
			CHECK(view.payload >= buf &&
			      view.payload + view.canonical_len <=
				      buf + length);
			unsigned char result[32];
			memset(result, 0x5a, sizeof(result));
			rc = audit_record_verify(&view, zero,
						 audit_digest_provider(alg),
						 result);
			if (rc)
				for (size_t i = 0; i < sizeof(result); ++i)
					CHECK(result[i] == 0x5a);
		}
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	char dir[] = "/tmp/audit-record-XXXXXX";
	enter_temp(dir);
	audit_integrity_t alg = crypto_algorithm(argv[2]);
	if (!strcmp(argv[1], "roundtrip"))
		roundtrip(alg);
	else if (!strcmp(argv[1], "bounds"))
		bounds(alg);
	else if (!strcmp(argv[1], "fuzz"))
		fuzz(alg);
	else
		malformed(argv[1], alg);
	audit_test_cleanup(dir);
	printf("record %s %s passed\n", argv[1], argv[2]);
	return 0;
}
