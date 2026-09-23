#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
/* argv: DSO followed by the exact public export names. */
int main(int argc, char **argv)
{
	if (argc < 3)
		return 1;
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
	for (int i = 2; i < argc; ++i) {
		void *a = dlsym(h, argv[i]);
		void *b = dlvsym(h, argv[i], "LOGGER_0.9");
		if (!a || a != b) {
			fprintf(stderr, "missing/version error: %s\n", argv[i]);
			return 3;
		}
	}
	const char *private_names[] = {
		"logger_queue_init",	       "logger_wait_for_output",
		"logger_process_object_count", "logger_scope_enter",
		"audit_digest_provider",       "audit_checkpoint_persist",
		"logger_fault_should_fail"
	};
	for (unsigned i = 0;
	     i < sizeof(private_names) / sizeof(private_names[0]); ++i)
		if (dlsym(h, private_names[i])) {
			fprintf(stderr, "private symbol exposed\n");
			return 4;
		}
	return dlclose(h) ? 5 : 0;
}
