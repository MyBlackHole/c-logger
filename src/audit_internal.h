#ifndef AUDIT_INTERNAL_H
#define AUDIT_INTERNAL_H
#include "audit.h"
#include <stddef.h>
#include <stdint.h>
#define AUDIT_PATH_MAX 4096
typedef struct {
	uint32_t algorithm;
	uint64_t seq;
	uint64_t offset;
	unsigned char hash[32];
} audit_ckpt_t;
typedef struct {
	audit_integrity_t algorithm;
	const char *name;
	size_t digest_size;
	/* 0 / -errno. Output remains unchanged on error. NULL input requires len=0. */
	int (*hash)(const void *, size_t, unsigned char[32]);
} audit_digest_ops_t;
const audit_digest_ops_t *audit_digest_provider(audit_integrity_t);
void audit_digest_hex(const unsigned char *, size_t, char *);
/* Digest preflight; no zero-digest fallback and no file effects. */
int audit_digest_check(const audit_digest_ops_t *);
void audit_hash_hex(const unsigned char *, char[65]);
uint32_t audit_crc32(const void *, size_t);
int audit_checkpoint_load(const char *, audit_ckpt_t *);
int audit_checkpoint_persist(const char *, const audit_ckpt_t *);
int audit_recover_set(const char *dir, const char *name, const char *active,
		      audit_ckpt_t *);
#endif
