#define _GNU_SOURCE
#include "record_support.h"

static void checkpoint_case(const char *mode)
{
	audit_ckpt_t cp = { .algorithm = AUDIT_INTEGRITY_SHA256,
			    .seq = 3,
			    .offset = 900 };
	memset(cp.hash, 0xab, 32);
	CHECK(audit_checkpoint_persist("app.audit.state", &cp) == 0);
	crypto_file_t saved = crypto_snapshot("app.audit.state");
	char s[1024];
	CHECK(saved.size < sizeof(s));
	memcpy(s, saved.data, saved.size);
	s[saved.size] = 0;
	int error = EBADMSG, recalc = 1;
	if (!strcmp(mode, "ok"))
		error = 0;
	else if (!strcmp(mode, "suffix"))
		record_replace(s, sizeof(s), "\n", " extra=1\n");
	else if (!strcmp(mode, "second-line"))
		record_replace(s, sizeof(s), "\n", "\nSECOND\n");
	else if (!strcmp(mode, "second-blank"))
		record_replace(s, sizeof(s), "\n", "\n\n");
	else if (!strcmp(mode, "negative-offset"))
		record_replace(s, sizeof(s), "offset=900", "offset=-1");
	else if (!strcmp(mode, "large-offset"))
		record_replace(s, sizeof(s), "offset=900",
			       "offset=18446744073709551615");
	else if (!strcmp(mode, "leading-seq"))
		record_replace(s, sizeof(s), "seq=3", "seq=03");
	else if (!strcmp(mode, "unknown-alg"))
		record_replace(s, sizeof(s), "alg=1", "alg=9");
	else if (!strcmp(mode, "short-hash"))
		record_replace(s, sizeof(s), "hash=ab", "hash=");
	else if (!strcmp(mode, "long-hash"))
		record_replace(s, sizeof(s), "hash=ab", "hash=ab00");
	else if (!strcmp(mode, "short-crc")) {
		char *p = strstr(s, " crc=");
		CHECK(p);
		p[12] = '\n';
		p[13] = 0;
		recalc = 0;
	} else if (!strcmp(mode, "unknown-version"))
		record_replace(s, sizeof(s), "version=2", "version=99");
	else if (!strcmp(mode, "missing-lf")) {
		s[saved.size - 1u] = 0;
		recalc = 0;
	} else if (!strcmp(mode, "nul"))
		recalc = 0;
	else if (!strcmp(mode, "bad-crc")) {
		char *p = strstr(s, " crc=");
		CHECK(p);
		p[5] = p[5] == '0' ? '1' : '0';
		recalc = 0;
	} else
		CHECK(0);
	if (recalc) {
		char *p = strstr(s, " crc=");
		CHECK(p);
		uint32_t crc = audit_crc32(s, (size_t)(p - s));
		char digits[9];
		CHECK(snprintf(digits, sizeof(digits), "%08x", crc) == 8);
		memcpy(p + 5, digits, 8);
	}
	size_t n = strlen(s);
	if (!strcmp(mode, "nul"))
		s[n - 2u] = 0;
	record_write("app.audit.state", s, n);
	audit_ckpt_t out, before;
	memset(&out, 0x5a, sizeof(out));
	memcpy(&before, &out, sizeof(before));
	int rc = audit_checkpoint_load("app.audit.state", &out);
	if (error) {
		crypto_expect_error(rc, error);
		CHECK(!memcmp(&out, &before, sizeof(out)));
	} else {
		CHECK(rc == 0 && out.seq == cp.seq && out.offset == cp.offset &&
		      !memcmp(cp.hash, out.hash, 32));
	}
	free(saved.data);
}

typedef struct {
	unsigned calls;
	int interrupted;
} cookie_t;
static ssize_t error_reader(void *p, char *buf, size_t n)
{
	cookie_t *c = p;
	if (!c->calls++) {
		CHECK(n >= 3);
		memcpy(buf, "abc", 3);
		return 3;
	}
	errno = EIO;
	return -1;
}
static ssize_t eintr_reader(void *p, char *buf, size_t n)
{
	cookie_t *c = p;
	if (!c->interrupted++) {
		errno = EINTR;
		return -1;
	}
	if (!c->calls++) {
		CHECK(n >= 4);
		memcpy(buf, "abc\n", 4);
		return 4;
	}
	return 0;
}

static void reader_cases(void)
{
	char buffer[AUDIT_RECORD_MAX + 1u];
	size_t length;
	for (size_t n = AUDIT_RECORD_MAX - 1u; n <= AUDIT_RECORD_MAX + 1u;
	     ++n) {
		for (int lf = 0; lf <= 1; ++lf) {
			FILE *f = tmpfile();
			CHECK(f);
			for (size_t i = 0; i < n; ++i)
				CHECK(fputc(lf && i == n - 1u ? '\n' : 'x',
					    f) != EOF);
			rewind(f);
			int rc = audit_line_read(f, buffer, AUDIT_RECORD_MAX,
						 &length);
			if (n > AUDIT_RECORD_MAX)
				CHECK(rc == -EOVERFLOW);
			else
				CHECK(rc == (lf ? AUDIT_LINE_COMPLETE :
						  AUDIT_LINE_PARTIAL) &&
				      length == n);
			CHECK(fclose(f) == 0);
		}
	}
	cookie_t cookie = { 0 };
	FILE *f = fopencookie(&cookie, "r",
			      (cookie_io_functions_t){ .read = error_reader });
	CHECK(f);
	CHECK(audit_line_read(f, buffer, AUDIT_RECORD_MAX, &length) == -EIO);
	CHECK(fclose(f) == 0);
	cookie = (cookie_t){ 0 };
	f = fopencookie(&cookie, "r",
			(cookie_io_functions_t){ .read = eintr_reader });
	CHECK(f);
	CHECK(audit_line_read(f, buffer, AUDIT_RECORD_MAX, &length) ==
		      AUDIT_LINE_COMPLETE &&
	      length == 4);
	CHECK(audit_line_read(f, buffer, AUDIT_RECORD_MAX, &length) ==
	      AUDIT_LINE_EOF);
	CHECK(fclose(f) == 0);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char path[] = "/tmp/audit-reader-XXXXXX";
	enter_temp(path);
	if (!strcmp(argv[1], "reader"))
		reader_cases();
	else
		checkpoint_case(argv[1]);
	audit_test_cleanup(path);
	printf("reader/checkpoint %s passed\n", argv[1]);
	return 0;
}
