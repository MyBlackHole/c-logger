#ifndef LOGGER_UAPI_H
#define LOGGER_UAPI_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Linux-UAPI-style struct copy:
 * - caller must provide the complete known structure;
 * - a larger future structure is accepted only when the unknown tail is zero;
 * - non-zero unknown data is rejected with E2BIG instead of being ignored.
 *
 * The caller remains responsible for ensuring that src points to struct_size
 * readable bytes. This is a userspace library, not a kernel copy_from_user()
 * boundary.
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
