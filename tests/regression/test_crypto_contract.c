#include "crypto_support.h"
#include "crypto_boundary_vectors.h"

static const audit_digest_ops_t *digest;
static audit_integrity_t algorithm;

static void check_hash(const void *data, size_t size, const char *expected)
{
	unsigned char output[34];
	memset(output, 0xa5, sizeof(output));
	CHECK(digest->hash(data, size, output + 1) == 0);
	CHECK(output[0] == 0xa5 && output[33] == 0xa5);
	char hex[65];
	audit_hash_hex(output + 1, hex);
	CHECK(!strcmp(hex, expected));
}

static void known_answers(void)
{
	const char *empty =
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
	check_hash(NULL, 0, empty);
	check_hash("", 0, empty);
	check_hash(
		"abc", 3,
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *message =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	check_hash(
		message, strlen(message),
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	unsigned char *million = malloc(1000000);
	CHECK(million != NULL);
	memset(million, 'a', 1000000);
	check_hash(
		million, 1000000,
		"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	free(million);
}

static void boundaries(void)
{
	unsigned char data[8193];
	for (size_t i = 0; i < sizeof(data); ++i)
		data[i] = (unsigned char)((i * 131 + i / 7 + 17) & 255u);
	for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
		check_hash(data, vectors[i].size, vectors[i].sha256);
	/* One-shot hashing consumes all input before publishing output. */
	unsigned char expected[32], alias[256];
	memcpy(alias, data, sizeof(alias));
	CHECK(digest->hash(alias, sizeof(alias), expected) == 0);
	CHECK(digest->hash(alias, sizeof(alias), alias) == 0);
	CHECK(!memcmp(alias, expected, 32));
	printf("binary length fixtures checked: %zu\n",
	       sizeof(vectors) / sizeof(vectors[0]));
}

static void invalid_input(void)
{
	unsigned char output[32], before[32];
	memset(output, 0xa5, sizeof(output));
	memcpy(before, output, sizeof(before));
	CHECK(digest->hash(NULL, 1, output) == -EINVAL);
	CHECK(!memcmp(output, before, 32));
	CHECK(digest->hash("abc", 3, NULL) == -EINVAL);
#if SIZE_MAX > UINT64_MAX / 8u
	CHECK(digest->hash("x", (size_t)(UINT64_MAX / 8u + 1u), output) ==
	      -EOVERFLOW);
	CHECK(!memcmp(output, before, 32));
#endif
	CHECK(audit_digest_provider(AUDIT_INTEGRITY_NONE) == NULL);
	CHECK(audit_digest_provider((audit_integrity_t)999) == NULL);
	CHECK(audit_digest_check(NULL) == -EPROTONOSUPPORT);
	CHECK(audit_digest_check(digest) == 0);
}

static void *hash_worker(void *arg)
{
	(void)arg;
	unsigned char data[8193];
	for (size_t i = 0; i < sizeof(data); ++i)
		data[i] = (unsigned char)((i * 131 + i / 7 + 17) & 255u);
	for (size_t i = 0; i < 400; ++i) {
		size_t index = i % (sizeof(vectors) / sizeof(vectors[0]));
		check_hash(data, vectors[index].size, vectors[index].sha256);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	algorithm = crypto_algorithm(argv[2]);
	digest = audit_digest_provider(algorithm);
	CHECK(digest != NULL && digest->digest_size == 32);
	if (!strcmp(argv[1], "vectors"))
		known_answers();
	else if (!strcmp(argv[1], "boundaries"))
		boundaries();
	else if (!strcmp(argv[1], "invalid"))
		invalid_input();
	else {
		CHECK(!strcmp(argv[1], "threads"));
		pthread_t threads[8];
		for (size_t i = 0; i < 8; ++i)
			CHECK(pthread_create(&threads[i], NULL, hash_worker,
					     NULL) == 0);
		for (size_t i = 0; i < 8; ++i)
			CHECK(pthread_join(threads[i], NULL) == 0);
	}
	printf("%s %s %s passed\n", audit_crypto_backend(), argv[2], argv[1]);
	return 0;
}
