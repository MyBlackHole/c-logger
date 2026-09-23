#include "audit_internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Self-contained SHA-256. No external crypto dependency or runtime
 * backend selection. Each call owns its state on the stack; constants are
 * immutable. The common output contract is 0 / -errno, with the caller's
 * 32-byte output unchanged on error. A digest is never an error sentinel.
 * Inputs are byte strings; NULL is allowed only for a zero-length message.
 */
static int validate_input(const void *data, size_t size,
			  const unsigned char *out)
{
	if (!out || (!data && size))
		return -EINVAL;
	/* SHA-256 encodes a 64-bit bit length. Check before reading input. */
#if SIZE_MAX > UINT64_MAX / 8u
	if (size > UINT64_MAX / 8u)
		return -EOVERFLOW;
#endif
	return 0;
}

static uint32_t load_be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static uint32_t ror32(uint32_t x, unsigned n)
{
	n &= 31u;
	return n ? (x >> n) | (x << (32u - n)) : x;
}

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block(uint32_t h[8], const unsigned char p[64])
{
	uint32_t w[64];
	for (unsigned i = 0; i < 16; ++i)
		w[i] = load_be32(p + i * 4);
	for (unsigned i = 16; i < 64; ++i) {
		uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^
			      (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
	uint32_t e = h[4], f = h[5], g = h[6], x = h[7];
	for (unsigned i = 0; i < 64; ++i) {
		uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = x + s1 + ch + sha256_k[i] + w[i];
		uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		x = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += x;
}

/* SHA-256 uses 512-bit blocks and a big-endian 64-bit bit length. */
static int builtin_sha256(const void *data, size_t size, unsigned char out[32])
{
	int rc = validate_input(data, size, out);
	if (rc)
		return rc;
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	const unsigned char *p = data;
	size_t remaining = size;
	while (remaining >= 64) {
		sha256_block(h, p);
		p += 64;
		remaining -= 64;
	}
	unsigned char tail[128] = { 0 };
	if (remaining)
		memcpy(tail, p, remaining); /* no memcpy(NULL, 0) */
	tail[remaining] = 0x80;
	size_t padded = remaining < 56 ? 64 : 128;
	uint64_t bits = (uint64_t)size * 8u;
	for (unsigned i = 0; i < 8; ++i)
		tail[padded - 1 - i] = (unsigned char)(bits >> (8u * i));
	sha256_block(h, tail);
	if (padded == 128)
		sha256_block(h, tail + 64);
	for (unsigned i = 0; i < 8; ++i)
		store_be32(out + i * 4, h[i]);
	return 0;
}

void audit_digest_hex(const unsigned char *digest, size_t size, char *out)
{
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < size; ++i) {
		out[i * 2] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 15];
	}
	out[size * 2] = '\0';
}

void audit_hash_hex(const unsigned char *digest, char out[65])
{
	audit_digest_hex(digest, 32, out);
}

const audit_digest_ops_t *audit_digest_provider(audit_integrity_t algorithm)
{
	static const audit_digest_ops_t sha = { AUDIT_INTEGRITY_SHA256,
						"SHA-256/builtin", 32,
						builtin_sha256 };
	/* Keep the private adapter so writer/recovery/verifier share the same
     * checked contract. Unsupported on-disk or API IDs never select a fallback. */
	return algorithm == AUDIT_INTEGRITY_SHA256 ? &sha : NULL;
}

int audit_digest_check(const audit_digest_ops_t *digest)
{
	if (!digest || !digest->hash || digest->digest_size != 32)
		return -EPROTONOSUPPORT;
	unsigned char probe[32];
	/* Check the digest contract even for empty/no-replay streams, before any
     * repair or append. Keep failure propagation consistent across all paths. */
	return digest->hash(NULL, 0, probe);
}

const char *audit_crypto_backend(void)
{
	return "builtin";
}
