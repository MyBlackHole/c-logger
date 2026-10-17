#include "logger.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

static const char redacted[] = "[REDACTED]";

const char *logger_redact(void)
{
	return redacted;
}

int logger_mask_secret(const char *v, char *out, size_t cap, size_t pre,
		       size_t suf)
{
	int saved = errno;
	int rc = 0;

	if (!out || !cap) {
		rc = -EINVAL;
		goto done;
	}
	if (!v) {
		if (cap < sizeof(redacted)) {
			rc = -ENOSPC;
			goto done;
		}
		memcpy(out, redacted, sizeof(redacted));
		goto done;
	}
	size_t n = strlen(v);
	if (pre > n)
		pre = n;
	if (suf > n - pre)
		suf = n - pre;
	/* 绝不通过此辅助函数泄露整个值，包括空字符串。 */
	if (suf == n - pre) {
		pre = 0;
		suf = 0;
	}
	/* 三个遮挡字节和终止符都纳入容量；检查完成前不修改输出。 */
	if (pre > SIZE_MAX - 4u || suf > SIZE_MAX - 4u - pre) {
		rc = -EOVERFLOW;
		goto done;
	}
	size_t need = pre + suf + 4u;
	if (need > cap) {
		rc = -ENOSPC;
		goto done;
	}
	/* 两个源片段有序且不相交，目标片段之间保留三个遮挡字节。
	 * 向右移动先保存后缀，向左或原地先保存前缀；memmove 处理片段
	 * 自身的重叠。遮挡最后写入，避免短隐藏区扩展时提前覆盖后缀。
	 * 地址比较依赖受支持 Linux/ELF 平台的平坦 uintptr_t 地址模型。
	 */
	if ((uintptr_t)out > (uintptr_t)v) {
		if (suf)
			memmove(out + pre + 3u, v + n - suf, suf);
		if (pre)
			memmove(out, v, pre);
	} else {
		if (pre)
			memmove(out, v, pre);
		if (suf)
			memmove(out + pre + 3u, v + n - suf, suf);
	}
	memcpy(out + pre, "***", 3);
	out[need - 1u] = '\0';
done:
	errno = saved;
	return rc;
}
