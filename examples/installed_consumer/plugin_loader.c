#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	if (argc != 2)
		return 1;
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
	void *sym = dlsym(h, "plugin_run");
	int (*run)(void) = NULL;
	_Static_assert(sizeof(run) == sizeof(sym), "Linux dlsym pointer ABI");
	memcpy(&run, &sym, sizeof(run));
	if (!run) {
		dlclose(h);
		return 3;
	}
	int rc = run();
	if (dlclose(h))
		return 4;
	return rc;
}
