#ifndef AUDIT_RECORD_H
#define AUDIT_RECORD_H

#include "audit_internal.h"
#include <stdio.h>

/* Existing on-disk format, not a new schema: payload excludes logger prefix/LF.
 * The writer and both readers use these same bounds. No unbounded getline(). */
#define AUDIT_PAYLOAD_MAX 4095u
#define AUDIT_RECORD_MAX 4608u
#define AUDIT_CHECKPOINT_MAX 511u

typedef struct {
	const char *payload; /* borrowed slice of the input */
	size_t canonical_len; /* through prev=, excluding " hash=" */
	uint64_t seq;
	uint64_t txn;
	char instance[33];
	unsigned char previous[32];
	unsigned char hash[32];
} audit_record_view_t;

enum audit_line_result {
	AUDIT_LINE_EOF = 0,
	AUDIT_LINE_COMPLETE = 1,
	AUDIT_LINE_PARTIAL = 2
};
/* Reads at most limit+1 bytes; includes LF in length. A PARTIAL result always
 * follows actual EOF. Oversize, embedded NUL and stream error return -errno.
 * buffer must have at least limit+1 bytes (extra byte for a convenience NUL). */
int audit_line_read(FILE *, char *buffer, size_t limit, size_t *length);
/* Strict canonical KV grammar; output is changed only on success. */
int audit_record_parse_payload(const char *, size_t, int chained,
			       audit_record_view_t *);
int audit_record_parse_line(const char *, size_t, audit_record_view_t *);
int audit_record_verify(const audit_record_view_t *,
			const unsigned char previous[32],
			const audit_digest_ops_t *, unsigned char next[32]);
/* Encode the unhashed payload. All strings are quoted/escaped; no guessing of
 * secret contents. Retains old bytes for ordinary strings and existing escapes. */
int audit_payload_encode(const char instance[33], uint64_t seq,
			 const audit_event_t *, char *, size_t, size_t *used);
int audit_hex_parse(const char *, size_t bytes, unsigned char *);
int audit_checkpoint_parse(const char *, size_t, audit_ckpt_t *);
#endif
