#include "audit.h"
#include "../src/audit_internal.h"
#include <string.h>
static int check(audit_integrity_t a, const char *expect)
{
	const audit_digest_ops_t *d = audit_digest_provider(a);
	if (!d)
		return 1;
	unsigned char out[32];
	char hex[65];
	if (d->hash("abc", 3, out) != 0)
		return 1;
	audit_digest_hex(out, 32, hex);
	return strcmp(hex, expect) != 0;
}
int main(void)
{
	if (check(AUDIT_INTEGRITY_SHA256,
		  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
		return 1;
	const char *b = audit_crypto_backend();
	return (!b || strcmp(b, "builtin")) ? 3 : 0;
}
