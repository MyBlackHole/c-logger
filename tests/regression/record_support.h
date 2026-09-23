#ifndef RECORD_SUPPORT_H
#define RECORD_SUPPORT_H
#include "crypto_support.h"
#include "audit_record.h"

static const char record_instance[] = "0123456789abcdef0123456789abcdef";
static const char record_prefix[] =
	"2026-09-22T12:00:00.000123+0800 INFO  pid=123 tid=123 [AUDIT] ";

typedef struct {
	char data[AUDIT_RECORD_MAX + 2u];
	size_t length;
	unsigned char hash[32];
} record_fixture_t;

static inline record_fixture_t record_fixture(audit_integrity_t alg,
					      uint64_t seq,
					      const unsigned char prev[32],
					      const char *event)
{
	record_fixture_t r = { 0 };
	char payload[AUDIT_PAYLOAD_MAX + 1u], h[65], previous[65];
	audit_event_t e = audit_test_event(event);
	e.detail = "ordinary metadata";
	size_t used;
	CHECK(audit_payload_encode(record_instance, seq, &e, payload,
				   sizeof(payload) - 140u, &used) == 0);
	audit_hash_hex(prev, previous);
	CHECK(snprintf(payload + used, sizeof(payload) - used, " prev=%s",
		       previous) == 70);
	used += 70;
	CHECK(audit_digest_provider(alg)->hash(payload, used, r.hash) == 0);
	audit_hash_hex(r.hash, h);
	int n = snprintf(r.data, sizeof(r.data), "%s%s hash=%s\n",
			 record_prefix, payload, h);
	CHECK(n > 0 && (size_t)n < sizeof(r.data));
	r.length = (size_t)n;
	return r;
}

static inline void record_write(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	CHECK(fwrite(data, 1, n, f) == n && fclose(f) == 0);
}

static inline void record_append(const char *path, const void *data, size_t n)
{
	FILE *f = fopen(path, "ab");
	CHECK(f != NULL);
	CHECK(fwrite(data, 1, n, f) == n && fclose(f) == 0);
}

static inline void record_series(const char *path, record_fixture_t *records,
				 size_t first, size_t count)
{
	record_write(path, "", 0);
	for (size_t i = first; i < first + count; ++i)
		record_append(path, records[i].data, records[i].length);
}

static inline void record_replace(char *s, size_t cap, const char *from,
				  const char *to)
{
	char *p = strstr(s, from);
	CHECK(p != NULL);
	size_t a = strlen(from), b = strlen(to), rest = strlen(p + a);
	CHECK(strlen(s) - a + b + 1u <= cap);
	memmove(p + b, p + a, rest + 1u);
	memcpy(p, to, b);
}

/* Tests recompute a digest over malformed canonical syntax. Rejection must be
 * due to grammar, not simply because mutation changed a digest. */
static inline void record_rehash(char *s, audit_integrity_t alg)
{
	char *p = strstr(s, "instance="), *last = NULL, *at = p;
	CHECK(p != NULL);
	while ((at = strstr(at, " hash="))) {
		last = at;
		++at;
	}
	CHECK(last && strlen(last + 6) >= 64);
	unsigned char hash[32];
	char hex[65];
	CHECK(audit_digest_provider(alg)->hash(p, (size_t)(last - p), hash) ==
	      0);
	audit_hash_hex(hash, hex);
	memcpy(last + 6, hex, 64);
}
#endif
