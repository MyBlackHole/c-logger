#ifndef LOGGER_UAPI_H
#define LOGGER_UAPI_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Linux UAPI 风格的结构体复制：
 * - 调用者必须提供完整的已知结构体；
 * - 未来更大的结构体只有在未知尾部全为零时才被接受；
 * - 非零未知数据以 E2BIG 拒绝，而不是忽略。
 *
 * 调用者仍负责确保 src 指向 struct_size 个可读字节。
 * 这是用户态库，不是内核 copy_from_user() 边界。
 */
static inline int logger_uapi_copy_struct(void *dst, size_t known_size,
					  const void *src, uint32_t struct_size,
					  uint32_t version,
					  uint32_t expected_version)
{
	if (!dst || !src)
		return -EINVAL;
	if (version != expected_version)
		return -EPROTONOSUPPORT;
	if ((size_t)struct_size < known_size)
		return -EINVAL;

	memcpy(dst, src, known_size);
	if ((size_t)struct_size > known_size) {
		const unsigned char *tail =
			(const unsigned char *)src + known_size;
		size_t extra = (size_t)struct_size - known_size;
		for (size_t i = 0; i < extra; ++i)
			if (tail[i] != 0)
				return -E2BIG;
	}
	return 0;
}

#endif
