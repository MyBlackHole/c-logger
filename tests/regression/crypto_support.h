#ifndef LOGGER_CRYPTO_REGRESSION_SUPPORT_H
#define LOGGER_CRYPTO_REGRESSION_SUPPORT_H
#include "audit_internal.h"
#include "audit_support.h"

static inline audit_integrity_t crypto_algorithm(const char *name)
{
	CHECK(!strcmp(name, "sha256"));
	return AUDIT_INTEGRITY_SHA256;
}

typedef struct {
	unsigned char *data;
	size_t size;
} crypto_file_t;

static inline crypto_file_t crypto_snapshot(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	long len = ftell(f);
	CHECK(len >= 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	crypto_file_t saved = { .size = (size_t)len };
	saved.data = malloc(saved.size + 1);
	CHECK(saved.data != NULL);
	CHECK(fread(saved.data, 1, saved.size, f) == saved.size);
	CHECK(!ferror(f));
	CHECK(fclose(f) == 0);
	return saved;
}

static inline void crypto_same_file(const char *path,
				    const crypto_file_t *before)
{
	crypto_file_t after = crypto_snapshot(path);
	CHECK(after.size == before->size);
	CHECK(!memcmp(before->data, after.data, before->size));
	free(after.data);
}

static inline void crypto_restore(const char *path, const crypto_file_t *saved)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	CHECK(fwrite(saved->data, 1, saved->size, f) == saved->size);
	CHECK(fclose(f) == 0);
}

static inline void crypto_expect_error(int rc, int code)
{
	CHECK(rc == -1);
	CHECK(errno == code);
}
#endif
