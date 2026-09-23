#define _GNU_SOURCE
#include "audit_support.h"
#include "audit_internal.h"

static int mode, call_count, data_fd = -1;
static int temp_sync_seen, directory_sync_seen;
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_close(int);
int __real_rename(const char *, const char *);

ssize_t __wrap_write(int fd, const void *data, size_t n)
{
	if (n >= 11 && !memcmp(data, "AUDIT_STATE", 11))
		data_fd = fd;
	if (fd == data_fd && mode) {
		int call = call_count++;
		if (mode == 1 && call == 0) {
			errno = EIO;
			return -1;
		}
		if (mode == 2 && call == 0)
			return 0;
		if (mode == 3) {
			if (call == 0) {
				errno = EINTR;
				return -1;
			}
			if (call == 1)
				return __real_write(fd, data, n > 7 ? 7 : n);
		}
	}
	return __real_write(fd, data, n);
}
int __wrap_fsync(int fd)
{
	struct stat st;
	CHECK(fstat(fd, &st) == 0);
	if (S_ISDIR(st.st_mode)) {
		++directory_sync_seen;
		if (mode == 5) {
			errno = EIO;
			return -1;
		}
	} else {
		++temp_sync_seen;
		if (mode == 4) {
			errno = EIO;
			return -1;
		}
	}
	return __real_fsync(fd);
}
int __wrap_close(int fd)
{
	int rc = __real_close(fd);
	if (fd == data_fd && mode == 6) {
		data_fd = -1;
		errno = EIO;
		return -1;
	}
	return rc;
}
int __wrap_rename(const char *old, const char *path)
{
	if (mode == 7) {
		errno = EACCES;
		return -1;
	}
	return __real_rename(old, path);
}
static int fd_count(void)
{
	DIR *dir = opendir("/proc/self/fd");
	CHECK(dir != NULL);
	int count = 0;
	struct dirent *e;
	while ((e = readdir(dir)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			++count;
	CHECK(closedir(dir) == 0);
	return count;
}
static void no_temporary_files(void)
{
	DIR *dir = opendir(".");
	CHECK(dir != NULL);
	struct dirent *e;
	while ((e = readdir(dir)))
		CHECK(!strstr(e->d_name, ".tmp."));
	CHECK(closedir(dir) == 0);
}
int main(int argc, char **argv)
{
	CHECK(argc == 2);
	char dir[] = "/tmp/audit-checkpoint-io-XXXXXX";
	enter_temp(dir);
	const char *names[] = { "ok",	 "write",     "zero",  "short-eintr",
				"fsync", "dir-fsync", "close", "rename" };
	int chosen = -1;
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
		if (!strcmp(names[i], argv[1]))
			chosen = (int)i;
	CHECK(chosen >= 0);
	audit_ckpt_t cp = { .algorithm = AUDIT_INTEGRITY_SHA256,
			    .seq = 1,
			    .offset = 123 };
	memset(cp.hash, 0x11, sizeof(cp.hash));
	CHECK(audit_checkpoint_persist("app.audit.state", &cp) == 0);
	int count = fd_count();
	cp.seq = 2;
	memset(cp.hash, 0x22, sizeof(cp.hash));
	int success = chosen == 0 || chosen == 3;
	for (int i = 0; i < 12; ++i) {
		call_count = 0;
		temp_sync_seen = directory_sync_seen = 0;
		data_fd = -1;
		mode = chosen;
		int rc = audit_checkpoint_persist("app.audit.state", &cp);
		int error = errno;
		mode = 0;
		data_fd = -1;
		if (success)
			CHECK(rc == 0 && temp_sync_seen == 1 &&
			      directory_sync_seen == 1);
		else
			CHECK(rc == -1 &&
			      error == (chosen == 7 ? EACCES : EIO));
		CHECK(fd_count() == count);
		no_temporary_files();
	}
	audit_ckpt_t loaded;
	CHECK(audit_checkpoint_load("app.audit.state", &loaded) == 0);
	CHECK(loaded.seq == (uint64_t)((success || chosen == 5) ? 2 : 1));
	/* Even after rename+directory-sync failure, retrying the same head works. */
	CHECK(audit_checkpoint_persist("app.audit.state", &cp) == 0);
	CHECK(audit_checkpoint_load("app.audit.state", &loaded) == 0 &&
	      loaded.seq == 2);
	audit_test_cleanup(dir);
	return 0;
}
