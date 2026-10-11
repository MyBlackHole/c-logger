#define _GNU_SOURCE
#include "log_durability_support.h"
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--crash-worker")) {
		logger_config_t c = durability_config(argv[2]);
		logger_t *l = logger_create(&c);
		if (!l || setenv("LOGGER_CRASH_POINT", argv[2], 1))
			return 98;
		for (unsigned i = 0; i < 4096; ++i)
			if (durability_record(l, "CRASH_TARGET", i))
				return 99;
		return 96; /* Missing hook is a test failure. */
	}
	if (argc != 2)
		return 97;
	char dir[] = "/tmp/logger-crash-XXXXXX";
	if (!mkdtemp(dir) || chdir(dir))
		return 1;
	unsetenv("LOGGER_CRASH_POINT");
	logger_config_t c = durability_config(argv[1]);
	logger_t *l = logger_create(&c);
	if (!l || durability_baseline(l) || logger_destroy_status(l))
		return 2;
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n < 0 || (size_t)n >= sizeof(exe) - 1)
		return 3;
	exe[n] = 0;
	char *args[] = { exe, "--crash-worker", argv[1], NULL };
	pid_t child = fork();
	if (child < 0)
		return 4;
	if (!child) {
		execv(exe, args);
		_exit(127);
	}
	int status;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 197 || durability_verify())
		return 5;
	l = logger_create(&c);
	if (!l || durability_record(l, "AFTER_RESTART", 0) ||
	    logger_destroy_status(l) || durability_verify())
		return 6;
	DIR *d = opendir(".");
	if (!d)
		return 7;
	struct dirent *e;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
		    unlink(e->d_name))
			return 8;
	if (closedir(d) || chdir("/") || rmdir(dir))
		return 9;
	return 0;
}
