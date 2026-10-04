#define _GNU_SOURCE
#include "record_support.h"

#include <sys/resource.h>
#include <time.h>

static double elapsed_ms(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) * 1000.0 +
	       (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

static void archive_path(char out[128], unsigned index)
{
	int n = snprintf(out, 128, "app.audit.20261004T120000.%06uZ.log",
			 index);
	CHECK(n > 0 && n < 128);
}

static double recover(audit_ckpt_t *cp, int expected_errno)
{
	struct timespec begin, end;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
	errno = 0;
	int rc = audit_recover_set(".", "app", "app.audit.log", cp);
	int error = errno;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	if (expected_errno) {
		CHECK(rc == -1);
		CHECK(error == expected_errno);
	} else {
		CHECK(rc == 0);
	}
	return elapsed_ms(&begin, &end);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char *end = NULL;
	errno = 0;
	unsigned long parsed = strtoul(argv[1], &end, 10);
	CHECK(!errno && end && !*end);
	CHECK(parsed > 0 && parsed <= 4096u);
	unsigned archives = (unsigned)parsed;

	char tmp[] = "/tmp/audit-recovery-capacity-XXXXXX";
	enter_temp(tmp);

	unsigned char previous[32] = { 0 };
	char middle_path[128] = { 0 };
	record_fixture_t middle_record = { 0 };
	record_fixture_t active_record = { 0 };

	struct timespec setup_begin, setup_end;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &setup_begin) == 0);
	for (unsigned i = 0; i < archives; ++i) {
		record_fixture_t record =
			record_fixture(AUDIT_INTEGRITY_SHA256, (uint64_t)i + 1u,
				       previous, "CAPACITY");
		char path[128];
		archive_path(path, i);
		record_write(path, record.data, record.length);
		memcpy(previous, record.hash, sizeof(previous));
		if (i == archives / 2u) {
			CHECK(snprintf(middle_path, sizeof(middle_path), "%s",
				       path) > 0);
			middle_record = record;
		}
	}
	active_record = record_fixture(AUDIT_INTEGRITY_SHA256,
				       (uint64_t)archives + 1u, previous,
				       "CAPACITY_ACTIVE");
	record_write("app.audit.log", active_record.data, active_record.length);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &setup_end) == 0);

	audit_ckpt_t genesis = { .algorithm = AUDIT_INTEGRITY_SHA256 };
	audit_ckpt_t cp = genesis;
	double valid_ms = recover(&cp, 0);
	CHECK(cp.seq == (uint64_t)archives + 1u);
	CHECK(cp.offset == active_record.length);
	CHECK(!memcmp(cp.hash, active_record.hash, sizeof(cp.hash)));

	CHECK(unlink(middle_path) == 0);
	cp = genesis;
	double missing_ms = recover(&cp, EBADMSG);
	record_write(middle_path, middle_record.data, middle_record.length);

	middle_record.data[middle_record.length / 2u] ^= 1;
	record_write(middle_path, middle_record.data, middle_record.length);
	cp = genesis;
	double corrupt_ms = recover(&cp, EBADMSG);
	middle_record.data[middle_record.length / 2u] ^= 1;
	record_write(middle_path, middle_record.data, middle_record.length);

	CHECK(truncate(middle_path, (off_t)middle_record.length - 1) == 0);
	cp = genesis;
	double truncated_ms = recover(&cp, EBADMSG);
	record_write(middle_path, middle_record.data, middle_record.length);

	struct rusage usage;
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
	double setup_ms = elapsed_ms(&setup_begin, &setup_end);

	printf("{\n");
	printf("  \"archives\": %u,\n", archives);
	printf("  \"records\": %u,\n", archives + 1u);
	printf("  \"setup_ms\": %.3f,\n", setup_ms);
	printf("  \"valid_recovery_ms\": %.3f,\n", valid_ms);
	printf("  \"missing_recovery_ms\": %.3f,\n", missing_ms);
	printf("  \"corrupt_recovery_ms\": %.3f,\n", corrupt_ms);
	printf("  \"truncated_recovery_ms\": %.3f,\n", truncated_ms);
	printf("  \"maxrss_kb\": %ld\n", usage.ru_maxrss);
	printf("}\n");

	audit_test_cleanup(tmp);
	return 0;
}
