#include "audit_record.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	const char *p, *end;
} cursor_t;

static int literal(cursor_t *c, const char *s)
{
	size_t n = strlen(s);
	if ((size_t)(c->end - c->p) < n || memcmp(c->p, s, n))
		return -EBADMSG;
	c->p += n;
	return 0;
}

static int lower_hex(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int audit_hex_parse(const char *s, size_t bytes, unsigned char *out)
{
	for (size_t i = 0; i < bytes; ++i) {
		int hi = lower_hex((unsigned char)s[2 * i]);
		int lo = lower_hex((unsigned char)s[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -EBADMSG;
		out[i] = (unsigned char)((hi << 4) | lo);
	}
	return 0;
}

static int hex(cursor_t *c, unsigned char *out, size_t bytes)
{
	if ((size_t)(c->end - c->p) < bytes * 2)
		return -EBADMSG;
	if (audit_hex_parse(c->p, bytes, out))
		return -EBADMSG;
	c->p += bytes * 2;
	return 0;
}

static int number(cursor_t *c, uint64_t max, uint64_t *value)
{
	const char *begin = c->p;
	uint64_t v = 0;
	while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
		unsigned digit = (unsigned)(*c->p - '0');
		if (v > max / 10 || (v == max / 10 && digit > max % 10))
			return -EBADMSG;
		v = v * 10 + digit;
		++c->p;
	}
	if (c->p == begin || (c->p - begin > 1 && *begin == '0'))
		return -EBADMSG;
	*value = v;
	return 0;
}

static int signed_error(cursor_t *c)
{
	int negative = c->p < c->end && *c->p == '-';
	if (negative)
		++c->p;
	uint64_t v;
	if (number(c, (uint64_t)INT_MAX + (unsigned)negative, &v))
		return -EBADMSG;
	return negative && !v ? -EBADMSG : 0;
}

static int quoted(cursor_t *c, int nonempty)
{
	if (literal(c, "\""))
		return -EBADMSG;
	size_t decoded = 0;
	while (c->p < c->end) {
		unsigned char ch = (unsigned char)*c->p++;
		if (ch == '"')
			return nonempty && !decoded ? -EBADMSG : 0;
		if (ch < 0x20 || ch == 0x7f)
			return -EBADMSG;
		if (ch == '\\') {
			if (c->p == c->end)
				return -EBADMSG;
			ch = (unsigned char)*c->p++;
			if (ch == 'x') {
				unsigned char byte;
				if (hex(c, &byte, 1))
					return -EBADMSG;
				/* Exactly the extra control-byte escapes emitted by writer.
                 * \0 is not representable in a C string input. */
				if (!byte || byte == '\n' || byte == '\r' ||
				    byte == '\t' ||
				    (byte >= 0x20 && byte != 0x7f))
					return -EBADMSG;
			} else if (ch != '\\' && ch != '"' && ch != 'n' &&
				   ch != 'r' && ch != 't') {
				return -EBADMSG;
			}
		}
		++decoded;
	}
	return -EBADMSG;
}

int audit_record_parse_payload(const char *s, size_t n, int chained,
			       audit_record_view_t *output)
{
	if (!s || !output || (chained != 0 && chained != 1))
		return -EINVAL;
	if (!n || n > AUDIT_PAYLOAD_MAX || memchr(s, 0, n))
		return -EBADMSG;
	cursor_t c = { s, s + n };
	audit_record_view_t r = { .payload = s };
	unsigned char instance[16];
	if (literal(&c, "instance="))
		return -EBADMSG;
	const char *instance_start = c.p;
	if (hex(&c, instance, sizeof(instance)))
		return -EBADMSG;
	memcpy(r.instance, instance_start, 32);
	if (literal(&c, " seq=") || number(&c, UINT64_MAX, &r.seq) || !r.seq ||
	    literal(&c, " txn=") || number(&c, UINT64_MAX, &r.txn) ||
	    literal(&c, " phase="))
		return -EBADMSG;
	int is_result = c.p < c.end && *c.p == 'R';
	if (literal(&c, is_result ? "RESULT" : "ATTEMPT") ||
	    literal(&c, " event=") || quoted(&c, 1) || literal(&c, " actor=") ||
	    quoted(&c, 0) || literal(&c, " source=") || quoted(&c, 0) ||
	    literal(&c, " resource=") || quoted(&c, 0) ||
	    literal(&c, " operation=") || quoted(&c, 1))
		return -EBADMSG;
	if (is_result) {
		if (literal(&c, " result="))
			return -EBADMSG;
		if (literal(&c, c.p < c.end && *c.p == 'S' ? "SUCCESS" :
							     "FAILURE") ||
		    literal(&c, " error=") || signed_error(&c))
			return -EBADMSG;
	}
	if ((size_t)(c.end - c.p) >= 8 && !memcmp(c.p, " detail=", 8)) {
		c.p += 8;
		if (quoted(&c, 0))
			return -EBADMSG;
	}
	if (chained) {
		if (literal(&c, " prev=") || hex(&c, r.previous, 32))
			return -EBADMSG;
		r.canonical_len = (size_t)(c.p - s);
		if (literal(&c, " hash=") || hex(&c, r.hash, 32))
			return -EBADMSG;
	} else {
		r.canonical_len = n;
	}
	if (c.p != c.end)
		return -EBADMSG;
	*output = r;
	return 0;
}

static int decimal_fixed(const char *s, size_t n)
{
	int v = 0;
	for (size_t i = 0; i < n; ++i) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + s[i] - '0';
	}
	return v;
}

static int envelope(cursor_t *c)
{
	/* Ordinary Audit has this fixed presentation envelope. It is syntactically
     * checked but intentionally remains OUTSIDE the historical digest domain.
     * Also accept a bare canonical record for exported/offline verification. */
	if ((size_t)(c->end - c->p) >= 9 && !memcmp(c->p, "instance=", 9))
		return 0;
	if (c->end - c->p < 31)
		return -EBADMSG;
	const char *s = c->p;
	if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
	    s[16] != ':' || s[19] != '.' || (s[26] != '+' && s[26] != '-'))
		return -EBADMSG;
	int year = decimal_fixed(s, 4), month = decimal_fixed(s + 5, 2);
	int day = decimal_fixed(s + 8, 2), hour = decimal_fixed(s + 11, 2);
	int minute = decimal_fixed(s + 14, 2),
	    second = decimal_fixed(s + 17, 2);
	int fraction = decimal_fixed(s + 20, 6);
	int zone_h = decimal_fixed(s + 27, 2),
	    zone_m = decimal_fixed(s + 29, 2);
	static const int days[] = { 31, 28, 31, 30, 31, 30,
				    31, 31, 30, 31, 30, 31 };
	if (year < 1 || month < 1 || month > 12 || day < 1 || hour < 0 ||
	    hour > 23 || minute < 0 || minute > 59 || second < 0 ||
	    second > 60 || fraction < 0 || zone_h < 0 || zone_h > 23 ||
	    zone_m < 0 || zone_m > 59)
		return -EBADMSG;
	int leap = !(year % 4) && (year % 100 || !(year % 400));
	if (day > days[month - 1] + (month == 2 && leap))
		return -EBADMSG;
	c->p += 31;
	uint64_t pid, tid;
	if (literal(c, " INFO  pid=") || number(c, INT_MAX, &pid) || !pid ||
	    literal(c, " tid=") || number(c, INT_MAX, &tid) || !tid ||
	    literal(c, " [AUDIT] "))
		return -EBADMSG;
	return 0;
}

int audit_record_parse_line(const char *s, size_t n, audit_record_view_t *out)
{
	if (!s || !out)
		return -EINVAL;
	if (!n || n > AUDIT_RECORD_MAX || s[n - 1] != '\n' || memchr(s, 0, n))
		return -EBADMSG;
	cursor_t c = { s, s + n - 1 };
	if (envelope(&c))
		return -EBADMSG;
	return audit_record_parse_payload(c.p, (size_t)(c.end - c.p), 1, out);
}

int audit_record_verify(const audit_record_view_t *r,
			const unsigned char prev[32],
			const audit_digest_ops_t *digest,
			unsigned char next[32])
{
	if (!r || !prev || !digest || !next)
		return -EINVAL;
	if (memcmp(r->previous, prev, 32))
		return -EBADMSG;
	unsigned char current[32];
	int rc = digest->hash(r->payload, r->canonical_len, current);
	if (rc)
		return rc;
	if (memcmp(current, r->hash, 32))
		return -EBADMSG;
	memcpy(next, current, 32);
	return 0;
}

int audit_line_read(FILE *f, char *out, size_t limit, size_t *length)
{
	if (!f || !out || !length || !limit)
		return -EINVAL;
	size_t n = 0;
	for (;;) {
		errno = 0;
		int ch = fgetc(f);
		if (ch == EOF) {
			if (ferror(f)) {
				if (errno == EINTR) {
					clearerr(f);
					continue;
				}
				return -(errno ? errno : EIO);
			}
			out[n] = '\0';
			*length = n;
			return n ? AUDIT_LINE_PARTIAL : AUDIT_LINE_EOF;
		}
		if (n == limit)
			return -EOVERFLOW;
		if (!ch)
			return -EBADMSG;
		out[n++] = (char)ch;
		if (ch == '\n') {
			out[n] = '\0';
			*length = n;
			return AUDIT_LINE_COMPLETE;
		}
	}
}

static int append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
	if (*used >= cap)
		return -EOVERFLOW;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + *used, cap - *used, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= cap - *used)
		return -EOVERFLOW;
	*used += (size_t)n;
	return 0;
}

static int escaped(char *out, size_t cap, size_t *used, const char *value)
{
	if (!value)
		value = "";
	if (append(out, cap, used, "\""))
		return -EOVERFLOW;
	static const char digits[] = "0123456789abcdef";
	for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
		char repr[5] = { 0 };
		switch (*p) {
		case '\\':
			memcpy(repr, "\\\\", 3);
			break;
		case '"':
			memcpy(repr, "\\\"", 3);
			break;
		case '\n':
			memcpy(repr, "\\n", 3);
			break;
		case '\r':
			memcpy(repr, "\\r", 3);
			break;
		case '\t':
			memcpy(repr, "\\t", 3);
			break;
		default:
			if (*p < 0x20 || *p == 0x7f) {
				repr[0] = '\\';
				repr[1] = 'x';
				repr[2] = digits[*p >> 4];
				repr[3] = digits[*p & 15];
			} else
				repr[0] = (char)*p;
		}
		if (append(out, cap, used, "%s", repr))
			return -EOVERFLOW;
	}
	return append(out, cap, used, "\"");
}

static int field(char *out, size_t cap, size_t *used, const char *key,
		 const char *s)
{
	if (append(out, cap, used, " %s=", key))
		return -EOVERFLOW;
	return escaped(out, cap, used, s);
}

int audit_payload_encode(const char instance[33], uint64_t seq,
			 const audit_event_t *e, char *out, size_t cap,
			 size_t *length)
{
	if (!instance || !e || !out || !length || !cap || !seq || !e->event ||
	    !*e->event || !e->operation || !*e->operation ||
	    (unsigned)e->phase > AUDIT_PHASE_RESULT ||
	    (e->phase == AUDIT_PHASE_RESULT &&
	     (unsigned)e->result > AUDIT_FAILURE))
		return -EINVAL;
	if (strlen(instance) != 32)
		return -EINVAL;
	unsigned char id[16];
	if (audit_hex_parse(instance, 16, id))
		return -EINVAL;
	if (cap > AUDIT_PAYLOAD_MAX + 1u)
		cap = AUDIT_PAYLOAD_MAX + 1u;
	size_t used = 0;
	int rc = append(
		out, cap, &used,
		"instance=%s seq=%llu txn=%llu phase=%s event=", instance,
		(unsigned long long)seq, (unsigned long long)e->transaction_id,
		e->phase == AUDIT_PHASE_RESULT ? "RESULT" : "ATTEMPT");
	if (rc || escaped(out, cap, &used, e->event) ||
	    field(out, cap, &used, "actor", e->actor) ||
	    field(out, cap, &used, "source", e->source) ||
	    field(out, cap, &used, "resource", e->resource) ||
	    field(out, cap, &used, "operation", e->operation))
		return -EOVERFLOW;
	if (e->phase == AUDIT_PHASE_RESULT &&
	    append(out, cap, &used, " result=%s error=%d",
		   e->result == AUDIT_SUCCESS ? "SUCCESS" : "FAILURE",
		   e->error_code))
		return -EOVERFLOW;
	if (e->detail && field(out, cap, &used, "detail", e->detail))
		return -EOVERFLOW;
	audit_record_view_t check;
	rc = audit_record_parse_payload(out, used, 0, &check);
	if (rc)
		return rc;
	*length = used;
	return 0;
}

int audit_checkpoint_parse(const char *s, size_t n, audit_ckpt_t *out)
{
	if (!s || !out)
		return -EINVAL;
	if (!n || n > AUDIT_CHECKPOINT_MAX || s[n - 1] != '\n' ||
	    memchr(s, 0, n))
		return -EBADMSG;
	cursor_t c = { s, s + n - 1 };
	audit_ckpt_t cp = { 0 };
	uint64_t algorithm;
	if (literal(&c, "AUDIT_STATE version=2 alg=") ||
	    number(&c, UINT32_MAX, &algorithm) ||
	    !audit_digest_provider((audit_integrity_t)algorithm) ||
	    literal(&c, " seq=") || number(&c, UINT64_MAX, &cp.seq) ||
	    literal(&c, " offset=") || number(&c, INT64_MAX, &cp.offset) ||
	    literal(&c, " hash=") || hex(&c, cp.hash, 32))
		return -EBADMSG;
	cp.algorithm = (uint32_t)algorithm;
	size_t canonical = (size_t)(c.p - s);
	unsigned char crc[4];
	if (literal(&c, " crc=") || hex(&c, crc, 4) || c.p != c.end)
		return -EBADMSG;
	uint32_t expected = ((uint32_t)crc[0] << 24) |
			    ((uint32_t)crc[1] << 16) | ((uint32_t)crc[2] << 8) |
			    crc[3];
	if (audit_crc32(s, canonical) != expected)
		return -EBADMSG;
	*out = cp;
	return 0;
}
