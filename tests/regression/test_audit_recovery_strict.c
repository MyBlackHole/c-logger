#define _GNU_SOURCE
#include "record_support.h"

static const char a1[] = "app.audit.20260922T120000.000001Z.log";
static const char a2[] =
	"app.audit.20260922T110000.000002Z.log"; /* backwards wall clock */
static const char a3[] =
	"app.20260922T100000.000003Z.audit.log"; /* alternate exact layout */
static record_fixture_t records[20];

static uint64_t series_size(size_t first, size_t count)
{
	uint64_t n = 0;
	for (size_t i = first; i < first + count; ++i)
		n += records[i].length;
	return n;
}

static void expect_tail_evidence(const char *data, size_t length)
{
	DIR *d = opendir(".");
	CHECK(d);
	struct dirent *e;
	unsigned count = 0;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "app.audit.log.tail.", 19))
			continue;
		++count;
		struct stat st;
		CHECK(stat(e->d_name, &st) == 0 && (st.st_mode & 0777) == 0600);
		crypto_file_t tail = crypto_snapshot(e->d_name);
		CHECK(tail.size == length && !memcmp(tail.data, data, length));
		free(tail.data);
	}
	CHECK(closedir(d) == 0 && count == 1);
}

static void scenario(const char *mode, audit_integrity_t alg)
{
	const unsigned char zero[32] = { 0 };
	for (size_t i = 0; i < 20; ++i)
		records[i] = record_fixture(
			alg, i + 1u, i ? records[i - 1u].hash : zero, "EVENT");
	record_series("app.audit.log", records, 0, 3);
	audit_ckpt_t cp = { .algorithm = (uint32_t)alg,
			    .seq = 1,
			    .offset = records[0].length };
	memcpy(cp.hash, records[0].hash, 32);
	int error = 0, partial = 0, next_offset_zero = 0;
	uint64_t expected_seq = 3;
	const char residue[] = "partial-crash-record";
	if (!strcmp(mode, "forward")) { /* default: valid stale checkpoint */
	} else if (!strcmp(mode, "partial")) {
		record_append("app.audit.log", residue, sizeof(residue) - 1u);
		partial = 1;
	} else if (!strcmp(mode, "complete-no-lf")) {
		record_series("app.audit.log", records, 0, 2);
		record_append("app.audit.log", records[2].data,
			      records[2].length - 1u);
		error = EBADMSG;
	} else if (!strcmp(mode, "oversize-complete") ||
		   !strcmp(mode, "oversize-eof")) {
		record_series("app.audit.log", records, 0, 1);
		char big[20000];
		memset(big, 'X', sizeof(big));
		record_append("app.audit.log", big, sizeof(big));
		if (!strcmp(mode, "oversize-complete")) {
			record_append("app.audit.log", "\n", 1);
			record_append("app.audit.log", records[1].data,
				      records[1].length);
		}
		error = EOVERFLOW;
	} else if (!strcmp(mode, "malformed") || !strcmp(mode, "nul")) {
		record_series("app.audit.log", records, 0, 1);
		if (!strcmp(mode, "nul"))
			records[1].data[20] = 0;
		else
			record_replace(records[1].data, sizeof(records[1].data),
				       " error=0", " error=1");
		record_append("app.audit.log", records[1].data,
			      records[1].length);
		record_append("app.audit.log", residue, sizeof(residue) - 1u);
		error = EBADMSG;
	} else if (!strcmp(mode, "checkpoint-offset")) {
		--cp.offset;
		error = EBADMSG;
	} else if (!strcmp(mode, "checkpoint-ahead")) {
		cp.offset = 900000;
		error = EBADMSG;
	} else if (!strcmp(mode, "checkpoint-seq")) {
		cp.seq = 9;
		error = EBADMSG;
	} else if (!strcmp(mode, "before-checkpoint-corrupt")) {
		cp.seq = 3;
		cp.offset = series_size(0, 3);
		memcpy(cp.hash, records[2].hash, 32);
		records[0].data[records[0].length - 3u] ^= 1;
		record_series("app.audit.log", records, 0, 3);
		error = EBADMSG;
	} else if (!strcmp(mode, "names")) {
		record_write("app.", "ignored", 7);
		record_write("app.audit.", "ignored", 7);
		record_write("app.20260922", "ignored", 7);
		record_write("app.audit.20260922T120000.000001Z.log.old",
			     "ignored", 7);
	} else {
		record_series(a1, records, 0, 3);
		record_series(a2, records, 3, 2);
		record_series("app.audit.log", records, 5, 1);
		expected_seq = 6;
		if (!strcmp(mode, "archives") ||
		    !strcmp(mode, "time-backwards")) {
			record_series(a3, records, 5, 2);
			record_series("app.audit.log", records, 7, 1);
			expected_seq = 8;
		} else if (!strcmp(mode, "active-larger")) {
			record_series("app.audit.log", records, 5, 15);
			expected_seq = 20;
			CHECK(file_size("app.audit.log") > (off_t)cp.offset);
		} else if (!strcmp(mode, "no-active") ||
			   !strcmp(mode, "empty-active")) {
			if (!strcmp(mode, "no-active"))
				CHECK(unlink("app.audit.log") == 0);
			else
				record_write("app.audit.log", "", 0);
			expected_seq = 5;
			next_offset_zero = 1;
		} else if (!strcmp(mode, "archive-partial")) {
			record_append(a2, residue, sizeof(residue) - 1u);
			error = EBADMSG;
		} else if (!strcmp(mode, "missing-middle")) {
			CHECK(unlink(a2) == 0);
			error = EBADMSG;
		} else if (!strcmp(mode, "duplicate")) {
			record_series(a3, records, 0, 3);
			error = EBADMSG;
		} else if (!strcmp(mode, "branch")) {
			record_fixture_t branch = record_fixture(
				alg, 4, records[2].hash, "BRANCH");
			record_write("app.audit.log", branch.data,
				     branch.length);
			error = EBADMSG;
		} else if (!strcmp(mode, "active-not-last")) {
			record_series(a2, records, 4, 2);
			record_series("app.audit.log", records, 3, 1);
			error = EBADMSG;
		} else if (!strcmp(mode, "lost-state-genesis")) {
			cp = (audit_ckpt_t){ .algorithm = (uint32_t)alg };
		} else if (!strcmp(mode, "lost-state-retained") ||
			   !strcmp(mode, "retained-anchor")) {
			CHECK(unlink(a1) == 0);
			cp = (audit_ckpt_t){ .algorithm = (uint32_t)alg };
			if (!strcmp(mode, "retained-anchor")) {
				cp.seq = 5;
				cp.offset = series_size(3, 2);
				memcpy(cp.hash, records[4].hash, 32);
			} else
				error = EBADMSG;
		} else if (!strcmp(mode, "symlink")) {
			CHECK(symlink(a1, a3) == 0);
			error = ELOOP;
		} else if (!strcmp(mode, "fifo")) {
			CHECK(mkfifo(a3, 0600) == 0);
			error = EINVAL;
		} else
			CHECK(0);
	}
	CHECK(audit_checkpoint_persist("app.audit.state", &cp) == 0);
	crypto_file_t state = crypto_snapshot("app.audit.state");
	int has_active = access("app.audit.log", F_OK) == 0;
	crypto_file_t log = { 0 };
	if (has_active)
		log = crypto_snapshot("app.audit.log");
	audit_ckpt_t original = cp;
	int rc = audit_recover_set(".", "app", "app.audit.log", &cp);
	if (error) {
		crypto_expect_error(rc, error);
		CHECK(!memcmp(&cp, &original, sizeof(cp)));
		if (has_active)
			crypto_same_file("app.audit.log", &log);
	} else {
		CHECK(rc == 0 && cp.seq == expected_seq &&
		      !memcmp(cp.hash, records[expected_seq - 1u].hash, 32));
		CHECK(cp.offset ==
		      (next_offset_zero ?
			       0u :
			       (uint64_t)file_size("app.audit.log")));
		if (partial) {
			CHECK(file_size("app.audit.log") ==
			      (off_t)series_size(0, 3));
			expect_tail_evidence(residue, sizeof(residue) - 1u);
		}
		/* Return value is a candidate only; direct recovery never writes state. */
		CHECK(audit_checkpoint_persist("next.state", &cp) == 0);
		CHECK(audit_recover_set(".", "app", "app.audit.log", &cp) == 0);
	}
	crypto_same_file("app.audit.state", &state);
	free(log.data);
	free(state.data);
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	char path[] = "/tmp/audit-recovery-strict-XXXXXX";
	enter_temp(path);
	scenario(argv[1], crypto_algorithm(argv[2]));
	audit_test_cleanup(path);
	printf("recovery %s %s passed\n", argv[1], argv[2]);
	return 0;
}
