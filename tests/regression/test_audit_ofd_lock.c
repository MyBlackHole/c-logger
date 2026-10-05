#define _GNU_SOURCE
#include "audit_support.h"

static void close_isolation(void)
{
	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == 0);

	/* POSIX process-associated locks may be released when this process closes
	 * any other fd for the same inode. The Audit OFD lease must survive it. */
	int unrelated = open("app.audit.lock", O_RDWR | O_CLOEXEC);
	CHECK(unrelated >= 0);
	CHECK(close(unrelated) == 0);

	/* A legacy POSIX contender must still conflict with the OFD holder. */
	check_process_lock("busy");
	CHECK(audit_shutdown_status() == 0);
	check_process_lock("free");
}

static void posix_conflict(void)
{
	int ready[2], release[2];
	CHECK(pipe(ready) == 0);
	CHECK(pipe(release) == 0);

	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(ready[0]);
		close(release[1]);
		int fd = open("app.audit.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (fd < 0)
			_exit(20);
		struct flock lk = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
		if (fcntl(fd, F_SETLK, &lk) != 0)
			_exit(21);
		char byte = 'R';
		if (write(ready[1], &byte, 1) != 1)
			_exit(22);
		if (read(release[0], &byte, 1) != 1)
			_exit(23);
		if (close(fd) != 0)
			_exit(24);
		_exit(0);
	}

	CHECK(close(ready[1]) == 0);
	CHECK(close(release[0]) == 0);
	char byte;
	CHECK(read(ready[0], &byte, 1) == 1);

	audit_config_t c = audit_test_config();
	CHECK(audit_init(&c) == -1 && errno == EBUSY);

	CHECK(write(release[1], "X", 1) == 1);
	CHECK(close(release[1]) == 0);
	CHECK(close(ready[0]) == 0);
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	CHECK(audit_init(&c) == 0);
	CHECK(audit_shutdown_status() == 0);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--probe-lock"))
		return audit_probe_lock(argv[2]);

	CHECK(argc == 2);
	char dir[] = "/tmp/audit-ofd-lock-XXXXXX";
	enter_temp(dir);

	if (!strcmp(argv[1], "close-isolation"))
		close_isolation();
	else if (!strcmp(argv[1], "posix-conflict"))
		posix_conflict();
	else
		CHECK(0);

	audit_test_cleanup(dir);
	return 0;
}
