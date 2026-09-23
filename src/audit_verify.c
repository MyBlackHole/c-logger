#include "audit_record.h"
#include "logger_process.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_anchor(const char *s, unsigned char out[32])
{
	if (strlen(s) != 64)
		return -EINVAL;
	for (size_t i = 0; i < 32; ++i) {
		int a = hex_value(s[i * 2]), b = hex_value(s[i * 2 + 1]);
		if (a < 0 || b < 0)
			return -EINVAL;
		out[i] = (unsigned char)((a << 4) | b);
	}
	return 0;
}

static int verify_from(const char *path, const char *anchor, char final_hex[65],
		       audit_integrity_t algorithm)
{
	int ready = logger_process_ensure();
	if (ready) {
		errno = -ready;
		return -1;
	}
	if (!path) {
		errno = EINVAL;
		return -1;
	}
	unsigned char prev[32] = { 0 };
	int rc = anchor ? parse_anchor(anchor, prev) : 0;
	const audit_digest_ops_t *digest = audit_digest_provider(algorithm);
	if (!rc)
		rc = audit_digest_check(digest);
	if (rc) {
		errno = -rc;
		return -1;
	}
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[AUDIT_RECORD_MAX + 1u];
	int error = 0;
	for (;;) {
		size_t length;
		int kind = audit_line_read(f, line, AUDIT_RECORD_MAX, &length);
		if (kind == AUDIT_LINE_EOF)
			break;
		if (kind < 0 || kind == AUDIT_LINE_PARTIAL) {
			error = kind < 0 ? -kind : EBADMSG;
			break;
		}
		audit_record_view_t record;
		rc = audit_record_parse_line(line, length, &record);
		if (!rc)
			rc = audit_record_verify(&record, prev, digest, prev);
		if (rc) {
			error = -rc;
			break;
		}
	}
	if (fclose(f) && !error)
		error = errno ? errno : EIO;
	if (error) {
		errno = error; /* cleanup must not replace a crypto failure */
		return -1;
	}
	if (final_hex)
		audit_hash_hex(prev, final_hex);
	return 0;
}

int audit_verify_file_from(const char *path, const char *anchor,
			   char final_hex[65])
{
	return verify_from(path, anchor, final_hex, AUDIT_INTEGRITY_SHA256);
}

int audit_verify_file(const char *path)
{
	return verify_from(path, NULL, NULL, AUDIT_INTEGRITY_SHA256);
}

int audit_verify_file_with(const char *path, audit_integrity_t algorithm)
{
	return verify_from(path, NULL, NULL, algorithm);
}
