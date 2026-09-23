#include "logger.h"
#include <errno.h>
#include <string.h>
const char *logger_redact(void)
{
	return "[REDACTED]";
}
int logger_mask_secret(const char *v, char *out, size_t cap, size_t pre,
		       size_t suf)
{
	if (!out || cap == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!v) {
		if (cap < 11) {
			errno = ENOSPC;
			return -1;
		}
		memcpy(out, "[REDACTED]", 11);
		return 0;
	}
	size_t n = strlen(v);
	if (pre > n)
		pre = n;
	if (suf > n - pre)
		suf = n - pre;
	/* Never reveal the whole value through this helper. */
	if (pre + suf >= n) {
		pre = 0;
		suf = 0;
	}
	const char *mask = "***";
	size_t need = pre + 3 + suf + 1;
	if (need > cap) {
		errno = ENOSPC;
		return -1;
	}
	if (pre)
		memcpy(out, v, pre);
	memcpy(out + pre, mask, 3);
	if (suf)
		memcpy(out + pre + 3, v + n - suf, suf);
	out[need - 1] = 0;
	return 0;
}
