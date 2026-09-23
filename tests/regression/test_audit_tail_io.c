#define _GNU_SOURCE
#include "record_support.h"

static const char *mode;
static int armed, tail_fd = -1, seen_truncate, seen_evidence, short_calls;
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_ftruncate(int, off_t);

static int is_tail(int fd)
{
	char link[64], path[PATH_MAX];
	CHECK(snprintf(link, sizeof(link), "/proc/self/fd/%d", fd) > 0);
	ssize_t n = readlink(link, path, sizeof(path) - 1u);
	CHECK(n > 0);
	path[n] = 0;
	return strstr(path, ".tail.") != NULL;
}

ssize_t __wrap_write(int fd, const void *data, size_t n)
{
	if (armed && is_tail(fd)) {
		tail_fd = fd;
		if (!strcmp(mode, "write")) {
			errno = EIO;
			return -1;
		}
		if (!strcmp(mode, "zero"))
			return 0;
		if (!strcmp(mode, "short-eintr")) {
			if (!short_calls++) {
				errno = EINTR;
				return -1;
			}
			return __real_write(fd, data, n > 3 ? 3 : n);
		}
	}
	return __real_write(fd, data, n);
}

int __wrap_fsync(int fd)
{
	if (armed) {
		struct stat st;
		CHECK(fstat(fd, &st) == 0);
		if (is_tail(fd)) {
			seen_evidence = 1;
			if (!strcmp(mode, "evidence-fsync")) {
				errno = EIO;
				return -1;
			}
		}
		if (S_ISDIR(st.st_mode) && seen_evidence &&
		    !strcmp(mode, "directory-fsync")) {
			errno = EIO;
			return -1;
		}
		if (seen_truncate && !strcmp(mode, "active-fsync")) {
			errno = EIO;
			return -1;
		}
	}
	return __real_fsync(fd);
}

int __wrap_ftruncate(int fd, off_t offset)
{
	if (armed) {
		CHECK(seen_evidence && tail_fd >= 0);
		seen_truncate = 1;
		if (!strcmp(mode, "truncate")) {
			errno = EIO;
			return -1;
		}
	}
	return __real_ftruncate(fd, offset);
}

static int fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d);
	int count = 0;
	while (readdir(d))
		++count;
	CHECK(closedir(d) == 0);
	return count;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	mode = argv[1];
	char dir[] = "/tmp/audit-tail-io-XXXXXX";
	enter_temp(dir);
	const unsigned char zero[32] = { 0 };
	record_fixture_t r =
		record_fixture(AUDIT_INTEGRITY_SHA256, 1, zero, "EVENT");
	record_write("app.audit.log", r.data, r.length);
	const char tail[] = "partial-crash-record";
	record_append("app.audit.log", tail, sizeof(tail) - 1u);
	audit_ckpt_t cp = { .algorithm = AUDIT_INTEGRITY_SHA256,
			    .seq = 1,
			    .offset = r.length };
	memcpy(cp.hash, r.hash, 32);
	CHECK(audit_checkpoint_persist("app.audit.state", &cp) == 0);
	audit_ckpt_t before = cp;
	crypto_file_t log = crypto_snapshot("app.audit.log"),
		      state = crypto_snapshot("app.audit.state");
	int before_fds = fd_count();
	armed = 1;
	int rc = audit_recover_set(".", "app", "app.audit.log", &cp);
	int saved = errno;
	armed = 0;
	errno = saved;
	int success = !strcmp(mode, "ok") || !strcmp(mode, "short-eintr");
	if (success)
		CHECK(rc == 0 && seen_truncate &&
		      file_size("app.audit.log") == (off_t)r.length);
	else {
		crypto_expect_error(rc, EIO);
		CHECK(!memcmp(&before, &cp, sizeof(cp)));
		if (strcmp(mode, "active-fsync"))
			crypto_same_file("app.audit.log", &log);
		else
			CHECK(seen_truncate &&
			      file_size("app.audit.log") == (off_t)r.length);
	}
	crypto_same_file("app.audit.state", &state);
	CHECK(fd_count() == before_fds);
	/* Partial repair is a separate fs transaction: even if final active fsync
     * fails, the saved evidence survives and no candidate checkpoint is published. */
	if (seen_truncate) {
		DIR *d = opendir(".");
		CHECK(d);
		struct dirent *e;
		unsigned files = 0;
		while ((e = readdir(d)))
			if (strstr(e->d_name, ".tail.")) {
				crypto_file_t witness =
					crypto_snapshot(e->d_name);
				CHECK(witness.size == sizeof(tail) - 1u &&
				      !memcmp(witness.data, tail,
					      witness.size));
				free(witness.data);
				++files;
			}
		CHECK(closedir(d) == 0 && files == 1);
	}
	free(log.data);
	free(state.data);
	audit_test_cleanup(dir);
	printf("tail I/O %s passed\n", mode);
	return 0;
}
