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
	int saved = errno;
	int rc = -EINVAL;

	if (!out || !cap)
		goto done;
	if (!v) {
		if (cap < sizeof("[REDACTED]")) {
			rc = -ENOSPC;
			goto done;
		}
		memcpy(out, "[REDACTED]", sizeof("[REDACTED]"));
		rc = 0;
		goto done;
	}
	size_t n = strlen(v);

	if (pre > n)
		pre = n;
	if (suf > n - pre)
		suf = n - pre;
	/* 裁剪保证 pre + suf 不溢出；绝不通过参数组合完整披露原值。 */
	if (pre + suf >= n) {
		pre = 0;
		suf = 0;
	}
	size_t visible = pre + suf;

	if (visible > SIZE_MAX - sizeof("***")) {
		rc = -EOVERFLOW;
		goto done;
	}
	size_t need = visible + sizeof("***");

	if (need > cap) {
		rc = -ENOSPC;
		goto done;
	}
	/* Linux 平坦地址空间：按目的地址方向保全尚未读取的首尾片段。
	 * 向低地址移动先复制前缀；向高地址移动先复制后缀。
	 * 两段目标区不重叠，最后写遮挡串和终止符，避免覆盖源后缀。
	 */
	if ((uintptr_t)out <= (uintptr_t)v) {
		if (pre)
			memmove(out, v, pre);
		if (suf)
			memmove(out + pre + 3, v + n - suf, suf);
	} else {
		if (suf)
			memmove(out + pre + 3, v + n - suf, suf);
		if (pre)
			memmove(out, v, pre);
	}
	memcpy(out + pre, "***", 3);
	out[need - 1] = 0;
	rc = 0;
done:
	errno = saved;
	return rc;
}
